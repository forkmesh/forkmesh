const FLOOR_NOTES_ENDPOINT = "/api/world/floor-notes";
const FLOOR_NOTE_ID_RE = /^[a-f0-9]{32}$/;
const FLOOR_NOTE_NAME_RE = /^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/;
const FLOOR_NOTE_MENTION_RE = /(^|[^A-Za-z0-9._%+-])@([a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)\b/g;
const FLOOR_NOTE_NEAR_DISTANCE = 48;
const FLOOR_NOTE_MAX_VISIBLE = 24;
const FLOOR_NOTE_SPRITE_CACHE = 48;
const FLOOR_NOTE_RESYNC_DISTANCE = 1;
const FLOOR_NOTE_TAP_DELAY_MS = 320;
const FLOOR_NOTE_CHIP_MS = 5000;
const FLOOR_NOTE_SYNC_MS = 1000;
const FLOOR_NOTE_POLL_MS = 3 * 60 * 1000;
const FLOOR_NOTE_TEXT_MAX = 500;
const FLOOR_NOTE_SUGGESTIONS = 6;
const FLOOR_NOTE_ERRORS = {
  sign_in_required: "Sign in to leave a note.",
  note_rate_limited: "You're posting quickly. Wait a few seconds and try again.",
  note_limit_reached: "You have 20 notes on the floor. Delete one to add another.",
  note_thread_full: "This note's conversation is full.",
  note_too_long: `Notes are limited to ${FLOOR_NOTE_TEXT_MAX} characters.`,
  note_text_required: "Write something first.",
  invalid_note_position: "Notes can only be placed on the walkable floor.",
  note_not_found: "That note was removed.",
  not_note_author: "Only the author or an admin can delete this.",
  world_access_blocked: "Posting is unavailable from this connection.",
};

const FLOOR_NOTE_STYLE = `
.fm-floor-note-chip{position:fixed;z-index:94;transform:translate(-50%,calc(-100% - 12px));display:inline-flex;align-items:center;gap:6px;min-height:34px;padding:0 13px;border:1px solid #1f883d;border-radius:10px;background:#1f883d;color:#fff;font:600 13px/1 "ForkMesh Sans",-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;box-shadow:3px 3px 0 #13632c;cursor:pointer;white-space:nowrap}
.fm-floor-note-chip:hover{background:#1a7f37}
.fm-floor-note-chip:active{transform:translate(calc(-50% + 2px),calc(-100% - 10px));box-shadow:1px 1px 0 #115a27}
.fm-floor-note-chip[hidden],.fm-floor-note-panel[hidden],.fm-floor-note-suggest[hidden],.fm-floor-note-panel [hidden]{display:none}
.fm-floor-note-panel{position:fixed;z-index:95;right:18px;top:84px;width:min(380px,calc(100vw - 24px));max-height:calc(100dvh - 120px);display:flex;flex-direction:column;background:#fff;border:1px solid #e7e9ed;border-radius:18px;box-shadow:0 20px 48px -16px rgb(15 23 42 / .24),0 4px 12px rgb(15 23 42 / .06);color:#111827;font:13px/1.45 "ForkMesh Sans",-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;overflow:hidden}
.fm-floor-note-head{display:flex;align-items:center;gap:8px;padding:12px 10px 10px 16px;border-bottom:1px solid #eef0f3}
.fm-floor-note-head strong{flex:1;min-width:0;font-size:15px;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.fm-floor-note-panel button{font:inherit;cursor:pointer}
.fm-floor-note-close{border:0;background:transparent;color:#6b7280;font-size:18px;line-height:1;padding:4px 8px;border-radius:8px}
.fm-floor-note-close:hover{background:#f3f4f6;color:#111827}
.fm-floor-note-thread{flex:1;min-height:0;overflow:auto;padding:12px 16px;display:grid;gap:10px;align-content:start}
.fm-floor-note-message{padding:10px 12px;border:1px solid #eef0f3;border-radius:12px;background:#f9fafb}
.fm-floor-note-message.root{background:#fffbeb;border-color:#fde68a}
.fm-floor-note-meta{display:flex;align-items:center;gap:6px;margin-bottom:4px;color:#6b7280;font-size:12px}
.fm-floor-note-meta b{color:#111827;font-weight:600}
.fm-floor-note-meta b a{color:inherit;text-decoration:none}
.fm-floor-note-meta b a:hover,.fm-floor-note-meta b a:focus-visible{text-decoration:underline}
.fm-floor-note-meta button{margin-left:auto;border:0;background:transparent;color:#6b7280;padding:2px 6px;border-radius:6px;font-size:12px}
.fm-floor-note-meta button:hover{background:#fee2e2;color:#b91c1c}
.fm-floor-note-text{white-space:pre-wrap;overflow-wrap:anywhere}
.fm-floor-note-text .mention{color:#2563eb;font-weight:600}
.fm-floor-note-text a.mention{text-decoration:none}
.fm-floor-note-text a.mention:hover,.fm-floor-note-text a.mention:focus-visible{text-decoration:underline}
.fm-floor-note-empty{color:#6b7280}
.fm-floor-note-compose{position:relative;display:grid;gap:8px;padding:12px 16px 14px;border-top:1px solid #eef0f3}
.fm-floor-note-compose textarea{min-height:76px;max-height:180px;resize:vertical;border:1px solid #dfe3e8;border-radius:10px;padding:9px 11px;font:inherit;color:#111827;background:#fff;outline:0}
.fm-floor-note-compose textarea:focus{border-color:#3b82f6;box-shadow:0 0 0 3px rgb(59 130 246 / .22)}
.fm-floor-note-actions{display:flex;align-items:center;gap:8px}
.fm-floor-note-actions small{flex:1;color:#6b7280}
.fm-floor-note-send{min-height:32px;border:1px solid #1f883d;border-radius:9px;background:#1f883d;color:#fff;padding:0 13px;font-weight:600;box-shadow:3px 3px 0 #13632c}
.fm-floor-note-send:hover{background:#1a7f37}
.fm-floor-note-send:active{transform:translate(2px,2px);box-shadow:1px 1px 0 #115a27}
.fm-floor-note-send:disabled{opacity:.6;cursor:default}
.fm-floor-note-suggest{position:absolute;left:16px;right:16px;bottom:calc(100% - 4px);margin:0;padding:4px;list-style:none;background:#fff;border:1px solid #e4e7eb;border-radius:10px;box-shadow:0 8px 24px -8px rgb(15 23 42 / .2)}
.fm-floor-note-suggest button{display:block;width:100%;border:0;background:transparent;text-align:left;padding:6px 8px;border-radius:7px;color:#111827}
.fm-floor-note-suggest button:hover,.fm-floor-note-suggest button.on{background:#eff6ff;color:#1d4ed8}
.fm-floor-note-status{padding:0 16px 10px;color:#6b7280;font-size:12px}
@media (max-width:650px){.fm-floor-note-panel{left:12px;right:12px;top:auto;bottom:12px;width:auto;max-height:70dvh}}
`;

function escapeHTML(value) {
  return String(value ?? "").replace(
    /[&<>"']/g,
    (character) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[
        character
      ],
  );
}

const FLOOR_NOTE_BOT_NAMES = new Set(["forkbot", "agent", "claude", "codex"]);

export function floorNoteProfileUrl(name) {
  const handle = String(name || "").trim().replace(/^@/, "").toLowerCase();
  if (!FLOOR_NOTE_NAME_RE.test(handle) || FLOOR_NOTE_BOT_NAMES.has(handle)) {
    return "";
  }
  const host = String(globalThis.location?.hostname || "").toLowerCase();
  const origin = ["app.forkmesh.com", "forkmesh.com", "world.forkmesh.com"].includes(host)
    ? "https://forkmesh.com"
    : String(globalThis.location?.origin || "");
  return `${origin}/@${encodeURIComponent(handle)}`;
}

function profileLinkHTML(name, className = "") {
  const label = `@${escapeHTML(name)}`;
  const url = floorNoteProfileUrl(name);
  if (!url) return className ? `<span class="${className}">${label}</span>` : label;
  return `<a${className ? ` class="${className}"` : ""} href="${escapeHTML(url)}">${label}</a>`;
}

export function noteTextHTML(value) {
  return escapeHTML(value).replace(
    FLOOR_NOTE_MENTION_RE,
    (_, lead, name) => `${lead}${profileLinkHTML(name, "mention")}`,
  );
}

function relativeTime(at, now = Date.now()) {
  const seconds = Math.max(0, Math.round((now - Number(at || 0)) / 1000));
  if (seconds < 60) return "just now";
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)}h ago`;
  return `${Math.floor(seconds / 86400)}d ago`;
}

export function normalizeFloorNote(value) {
  const id = String(value?.id || "");
  const author = String(value?.author || "").toLowerCase();
  const text = String(value?.text || "").slice(0, FLOOR_NOTE_TEXT_MAX);
  const x = Number(value?.x);
  const y = Number(value?.y);
  const z = Number(value?.z);
  if (
    !FLOOR_NOTE_ID_RE.test(id) ||
    !FLOOR_NOTE_NAME_RE.test(author) ||
    !text ||
    ![x, y, z].every(Number.isFinite)
  ) {
    return null;
  }
  return {
    id,
    author,
    text,
    x,
    y,
    z,
    createdAt: Math.max(0, Number(value?.createdAt) || 0),
    updatedAt: Math.max(0, Number(value?.updatedAt) || 0),
    replyCount: Math.max(0, Math.floor(Number(value?.replyCount) || 0)),
    mine: value?.mine === true,
    canDelete: value?.canDelete === true,
  };
}

function normalizeReply(value) {
  const id = String(value?.id || "");
  const author = String(value?.author || "").toLowerCase();
  const text = String(value?.text || "").slice(0, FLOOR_NOTE_TEXT_MAX);
  if (!FLOOR_NOTE_ID_RE.test(id) || !FLOOR_NOTE_NAME_RE.test(author) || !text) {
    return null;
  }
  return {
    id,
    author,
    text,
    createdAt: Math.max(0, Number(value?.createdAt) || 0),
    canDelete: value?.canDelete === true,
  };
}

export function mentionQueryAt(text, caret) {
  const before = String(text || "").slice(0, Math.max(0, caret));
  const match = before.match(/(^|[^A-Za-z0-9._%+-])@([a-z0-9-]{0,62})$/i);
  return match ? { query: match[2].toLowerCase(), start: caret - match[2].length - 1 } : null;
}

function paintNoteCanvas(canvas, note) {
  const context = canvas.getContext("2d");
  const { width, height } = canvas;
  context.clearRect(0, 0, width, height);
  context.fillStyle = "rgba(15, 23, 42, 0.18)";
  context.fillRect(10, 12, width - 14, height - 44);
  context.fillStyle = note.mine ? "#dbeafe" : "#fef3c7";
  context.fillRect(4, 4, width - 14, height - 44);
  context.fillStyle = note.mine ? "#93c5fd" : "#fcd34d";
  context.fillRect(4, 4, width - 14, 16);
  context.fillStyle = "#111827";
  context.font = "600 22px 'ForkMesh Sans', sans-serif";
  context.textBaseline = "top";
  context.fillText(`@${note.author}`.slice(0, 22), 18, 30);
  context.font = "20px 'ForkMesh Sans', sans-serif";
  const words = note.text.replace(/\s+/g, " ").split(" ");
  const lines = [];
  let line = "";
  for (const word of words) {
    const next = line ? `${line} ${word}` : word;
    if (context.measureText(next).width > width - 44 && line) {
      lines.push(line);
      line = word;
      if (lines.length === 4) break;
    } else {
      line = next;
    }
  }
  if (lines.length < 4 && line) lines.push(line);
  if (lines.length === 4 && words.join(" ").length > lines.join(" ").length) {
    lines[3] = `${lines[3].replace(/.{0,2}$/, "")}…`;
  }
  context.fillStyle = "#374151";
  lines.forEach((text, index) => context.fillText(text, 18, 62 + index * 26));
  context.fillStyle = "#6b7280";
  context.font = "600 18px 'ForkMesh Sans', sans-serif";
  context.fillText(
    note.replyCount ? `💬 ${note.replyCount}` : "💬 Reply",
    18,
    height - 72,
  );
  context.fillStyle = note.mine ? "#2563eb" : "#b45309";
  context.fillRect(width / 2 - 13, height - 40, 6, 40);
}

export function createWorldFloorNotes({
  THREE,
  world,
  fetchJSON,
  postJSON,
  viewer = () => "",
  memberNames = () => [],
}) {
  const group = new THREE.Group();
  group.name = "world-floor-notes";
  world.scene.add(group);
  const notes = new Map();
  const sprites = new Map();
  const raycaster = new THREE.Raycaster();
  const pointer = new THREE.Vector2();
  const canvasElement = world.renderer.domElement;
  let pendingTap = 0;
  let chipTimer = 0;
  let chipPoint = null;
  let openId = "";
  let composing = null;
  let thread = null;
  let refreshTimer = 0;
  let loading = null;
  let disposed = false;
  let suggestions = [];
  let suggestionIndex = 0;

  if (!document.querySelector("style[data-floor-note-style]")) {
    const style = document.createElement("style");
    style.dataset.floorNoteStyle = "";
    style.textContent = FLOOR_NOTE_STYLE;
    document.head.append(style);
  }
  const chip = document.createElement("button");
  chip.type = "button";
  chip.className = "fm-floor-note-chip";
  chip.hidden = true;
  chip.dataset.floorNoteAdd = "";
  chip.textContent = "＋ Add a note here";
  const panel = document.createElement("section");
  panel.className = "fm-floor-note-panel";
  panel.hidden = true;
  panel.setAttribute("aria-label", "Floor note");
  panel.dataset.floorNotePanel = "";
  panel.innerHTML = `<header class="fm-floor-note-head"><strong data-floor-note-title>Note</strong><button type="button" class="fm-floor-note-close" data-floor-note-close aria-label="Close note">×</button></header><div class="fm-floor-note-thread" data-floor-note-thread></div><div class="fm-floor-note-compose"><ul class="fm-floor-note-suggest" data-floor-note-suggest hidden></ul><textarea name="floor-note-input" data-floor-note-input maxlength="${FLOOR_NOTE_TEXT_MAX}" placeholder="Write a note… type @ to tag someone"></textarea><div class="fm-floor-note-actions"><small data-floor-note-count></small><button type="button" class="fm-floor-note-send" data-floor-note-send>Post note</button></div></div><div class="fm-floor-note-status" data-floor-note-status hidden></div>`;
  document.body.append(chip, panel);
  const $ = (selector) => panel.querySelector(selector);
  const input = $("[data-floor-note-input]");
  const sendButton = $("[data-floor-note-send]");
  const suggestList = $("[data-floor-note-suggest]");

  function setStatus(message) {
    const status = $("[data-floor-note-status]");
    status.textContent = message || "";
    status.hidden = !message;
  }

  function errorCopy(error) {
    const code = String(error?.payload?.error || error?.code || "");
    return FLOOR_NOTE_ERRORS[code] || String(error?.message || "The note could not be saved.");
  }

  function spriteFor(note) {
    let entry = sprites.get(note.id);
    const signature = `${note.author}|${note.text}|${note.replyCount}|${note.mine}`;
    if (!entry) {
      const canvas = document.createElement("canvas");
      canvas.width = 256;
      canvas.height = 256;
      const texture = new THREE.CanvasTexture(canvas);
      texture.colorSpace = THREE.SRGBColorSpace;
      const material = new THREE.SpriteMaterial({
        map: texture,
        transparent: true,
        toneMapped: false,
        fog: false,
      });
      const sprite = new THREE.Sprite(material);
      sprite.center.set(0.5, 0);
      sprite.scale.set(1.5, 1.5, 1);
      sprite.userData.floorNoteId = note.id;
      sprite.name = `floor-note:${note.id}`;
      group.add(sprite);
      entry = { sprite, canvas, texture, material, signature: "" };
      sprites.set(note.id, entry);
    }
    if (entry.signature !== signature) {
      paintNoteCanvas(entry.canvas, note);
      entry.texture.needsUpdate = true;
      entry.signature = signature;
    }
    entry.sprite.position.set(note.x, note.y, note.z);
    return entry;
  }

  function removeSprite(id) {
    const entry = sprites.get(id);
    if (!entry) return;
    group.remove(entry.sprite);
    entry.texture.dispose();
    entry.material.dispose();
    sprites.delete(id);
  }

  const lastSync = { x: Infinity, z: Infinity, hidden: null, openId: "" };

  function trimSpriteCache(position) {
    if (sprites.size <= FLOOR_NOTE_SPRITE_CACHE) return;
    const idle = [];
    for (const [id, entry] of sprites) {
      if (!entry.sprite.visible) {
        const note = notes.get(id);
        const distance = note
          ? (note.x - position.x) ** 2 + (note.z - position.z) ** 2
          : Infinity;
        idle.push([distance, id]);
      }
    }
    idle.sort((left, right) => right[0] - left[0]);
    for (const [, id] of idle) {
      if (sprites.size <= FLOOR_NOTE_SPRITE_CACHE) break;
      removeSprite(id);
    }
  }

  function syncVisibility() {
    if (disposed) return;
    const hidden =
      world.isOfficeInterior?.() === true || world.isBeachScene?.() === true;
    group.visible = !hidden;
    lastSync.hidden = hidden;
    if (hidden) return;
    const position = world.getPosition?.() || { x: 0, z: 0 };
    lastSync.x = position.x;
    lastSync.z = position.z;
    lastSync.openId = openId || "";
    const limit = FLOOR_NOTE_NEAR_DISTANCE * FLOOR_NOTE_NEAR_DISTANCE;
    const nearby = [];
    for (const note of notes.values()) {
      const distance = (note.x - position.x) ** 2 + (note.z - position.z) ** 2;
      if (distance <= limit) nearby.push([distance, note.id]);
    }
    if (nearby.length > FLOOR_NOTE_MAX_VISIBLE) {
      nearby.sort((left, right) => left[0] - right[0]);
      nearby.length = FLOOR_NOTE_MAX_VISIBLE;
    }
    const shown = new Set();
    for (const [, id] of nearby) shown.add(id);
    if (openId && notes.has(openId)) shown.add(openId);
    for (const [id, entry] of sprites) {
      entry.sprite.visible = shown.has(id);
    }
    for (const id of shown) spriteFor(notes.get(id)).sprite.visible = true;
    trimSpriteCache(position);
  }

  function syncVisibilityTick() {
    if (disposed || document.hidden) return;
    const hidden =
      world.isOfficeInterior?.() === true || world.isBeachScene?.() === true;
    const position = world.getPosition?.() || { x: 0, z: 0 };
    const moved =
      (position.x - lastSync.x) ** 2 + (position.z - lastSync.z) ** 2 >=
      FLOOR_NOTE_RESYNC_DISTANCE * FLOOR_NOTE_RESYNC_DISTANCE;
    if (!moved && hidden === lastSync.hidden && (openId || "") === lastSync.openId) {
      return;
    }
    syncVisibility();
  }

  function applyList(payload) {
    const next = new Map();
    for (const value of Array.isArray(payload?.notes) ? payload.notes : []) {
      const note = normalizeFloorNote(value);
      if (note) next.set(note.id, note);
    }
    notes.clear();
    for (const [id, note] of next) notes.set(id, note);
    for (const id of [...sprites.keys()]) {
      if (!notes.has(id)) removeSprite(id);
    }
    syncVisibility();
  }

  async function refresh() {
    if (disposed) return;
    if (loading) return loading;
    loading = (async () => {
      try {
        applyList(
          await fetchJSON(FLOOR_NOTES_ENDPOINT, {
            timeout: 8000,
            cache: "no-store",
            dedupe: true,
          }),
        );
      } catch (_) {
      } finally {
        loading = null;
      }
    })();
    return loading;
  }

  function hideChip() {
    window.clearTimeout(chipTimer);
    chip.hidden = true;
    chipPoint = null;
  }

  function showChip(tap) {
    chipPoint = { x: tap.x, y: tap.y, z: tap.z };
    chip.style.left = `${Math.round(tap.clientX)}px`;
    chip.style.top = `${Math.round(tap.clientY)}px`;
    chip.hidden = false;
    window.clearTimeout(chipTimer);
    chipTimer = window.setTimeout(hideChip, FLOOR_NOTE_CHIP_MS);
  }

  function noteAt(clientX, clientY) {
    if (!group.visible || !sprites.size) return "";
    const rect = canvasElement.getBoundingClientRect();
    if (!rect.width || !rect.height) return "";
    pointer.set(
      ((clientX - rect.left) / rect.width) * 2 - 1,
      -((clientY - rect.top) / rect.height) * 2 + 1,
    );
    raycaster.setFromCamera(pointer, world.camera);
    const visible = [];
    for (const { sprite } of sprites.values()) {
      if (sprite.visible) visible.push(sprite);
    }
    if (!visible.length) return "";
    const hit = raycaster.intersectObjects(visible, false)[0];
    return String(hit?.object?.userData?.floorNoteId || "");
  }

  function tap(detail) {
    if (disposed || !detail) return;
    const id = noteAt(detail.clientX, detail.clientY);
    window.clearTimeout(pendingTap);
    hideChip();
    if (id) {
      void openNote(id);
      return;
    }
    pendingTap = window.setTimeout(() => {
      pendingTap = 0;
      showChip(detail);
    }, FLOOR_NOTE_TAP_DELAY_MS);
  }

  function cancelPendingTap() {
    window.clearTimeout(pendingTap);
    pendingTap = 0;
    hideChip();
  }

  function renderCount() {
    const length = input.value.length;
    $("[data-floor-note-count]").textContent =
      length > FLOOR_NOTE_TEXT_MAX - 80
        ? `${FLOOR_NOTE_TEXT_MAX - length} left`
        : "Visible to everyone in the World";
  }

  function renderThread() {
    const container = $("[data-floor-note-thread]");
    const signedIn = Boolean(viewer());
    if (composing) {
      $("[data-floor-note-title]").textContent = "New floor note";
      container.innerHTML = `<p class="fm-floor-note-empty">Leave a note on this spot. Everyone who walks by can read it and reply. Tag people with @name to ping them.</p>`;
      sendButton.textContent = "Post note";
    } else if (thread) {
      const { note, replies } = thread;
      $("[data-floor-note-title]").textContent = `Note from @${note.author}`;
      const message = (item, root) =>
        `<article class="fm-floor-note-message${root ? " root" : ""}" data-floor-note-message="${item.id}"><div class="fm-floor-note-meta"><b>${profileLinkHTML(item.author)}</b><span>${relativeTime(item.createdAt)}</span>${item.canDelete ? `<button type="button" data-floor-note-delete="${root ? "" : item.id}">Delete</button>` : ""}</div><div class="fm-floor-note-text">${noteTextHTML(item.text)}</div></article>`;
      container.innerHTML =
        message(note, true) +
        (replies.length
          ? replies.map((reply) => message(reply, false)).join("")
          : `<p class="fm-floor-note-empty">No replies yet. Start the conversation.</p>`);
      sendButton.textContent = "Reply";
      container.scrollTop = container.scrollHeight;
    } else {
      $("[data-floor-note-title]").textContent = "Note";
      container.innerHTML = `<p class="fm-floor-note-empty">Loading…</p>`;
    }
    input.disabled = !signedIn;
    sendButton.disabled = !signedIn;
    input.placeholder = signedIn
      ? composing
        ? "Write a note… type @ to tag someone"
        : "Reply… type @ to tag someone"
      : "Sign in to write or reply.";
    renderCount();
  }

  function showPanel() {
    panel.hidden = false;
    setStatus("");
    renderThread();
    syncVisibility();
  }

  function showGone(message) {
    openId = "";
    composing = null;
    thread = null;
    hideSuggestions();
    panel.hidden = false;
    $("[data-floor-note-title]").textContent = "Note";
    $("[data-floor-note-thread]").innerHTML =
      `<p class="fm-floor-note-empty">${escapeHTML(message)}</p>`;
    input.value = "";
    input.disabled = true;
    sendButton.disabled = true;
    setStatus("");
    syncVisibility();
  }

  function close() {
    panel.hidden = true;
    openId = "";
    composing = null;
    thread = null;
    hideSuggestions();
    syncVisibility();
  }

  function compose(point) {
    if (!point) return;
    composing = { ...point };
    openId = "";
    thread = null;
    input.value = "";
    showPanel();
    if (!viewer()) {
      setStatus(FLOOR_NOTE_ERRORS.sign_in_required);
      return;
    }
    input.focus();
  }

  function applyThread(payload) {
    const note = normalizeFloorNote(payload?.note);
    if (!note) return false;
    thread = {
      note,
      replies: (Array.isArray(payload?.replies) ? payload.replies : [])
        .map(normalizeReply)
        .filter(Boolean),
    };
    notes.set(note.id, note);
    openId = note.id;
    return true;
  }

  async function openNote(id) {
    const noteId = String(id || "").toLowerCase();
    if (!FLOOR_NOTE_ID_RE.test(noteId) || disposed) return false;
    composing = null;
    openId = noteId;
    if (thread?.note?.id !== noteId) thread = null;
    showPanel();
    try {
      const payload = await fetchJSON(
        `${FLOOR_NOTES_ENDPOINT}?id=${noteId}`,
        { timeout: 8000, cache: "no-store", dedupe: true },
      );
      if (openId !== noteId) return false;
      if (!applyThread(payload)) throw new Error("note_not_found");
      renderThread();
      syncVisibility();
      return true;
    } catch (error) {
      if (openId !== noteId) return false;
      if (Number(error?.status) === 404) {
        notes.delete(noteId);
        removeSprite(noteId);
        showGone(FLOOR_NOTE_ERRORS.note_not_found);
        return false;
      }
      setStatus(errorCopy(error));
      return false;
    }
  }

  async function submit() {
    const text = input.value.trim();
    if (!text) {
      setStatus(FLOOR_NOTE_ERRORS.note_text_required);
      return;
    }
    const body = composing
      ? { action: "create", text, x: composing.x, y: composing.y, z: composing.z }
      : thread
        ? { action: "reply", id: thread.note.id, text }
        : null;
    if (!body) return;
    sendButton.disabled = true;
    try {
      const payload = await postJSON(FLOOR_NOTES_ENDPOINT, body, {
        timeout: 12_000,
      });
      composing = null;
      input.value = "";
      hideSuggestions();
      if (applyThread(payload)) {
        renderThread();
        syncVisibility();
      }
      const tagged = Array.isArray(payload?.mentioned) ? payload.mentioned : [];
      setStatus(
        tagged.length
          ? `Pinged ${tagged.map((name) => `@${name}`).join(", ")}.`
          : "",
      );
      void refresh();
    } catch (error) {
      setStatus(errorCopy(error));
    } finally {
      sendButton.disabled = !viewer();
    }
  }

  async function remove(replyId) {
    if (!thread) return;
    const noteId = thread.note.id;
    const label = replyId ? "this reply" : "this note and its conversation";
    if (!window.confirm(`Delete ${label}?`)) return;
    try {
      const payload = await postJSON(
        FLOOR_NOTES_ENDPOINT,
        { action: "delete", id: noteId, ...(replyId ? { replyId } : {}) },
        { timeout: 12_000 },
      );
      if (replyId && applyThread(payload)) {
        renderThread();
      } else if (!replyId) {
        notes.delete(noteId);
        removeSprite(noteId);
        showGone("Note deleted.");
      }
      void refresh();
    } catch (error) {
      setStatus(errorCopy(error));
    }
  }

  function hideSuggestions() {
    suggestions = [];
    suggestList.hidden = true;
    suggestList.innerHTML = "";
  }

  function renderSuggestions() {
    const mention = mentionQueryAt(input.value, input.selectionStart ?? 0);
    if (!mention) {
      hideSuggestions();
      return;
    }
    const self = String(viewer() || "").toLowerCase();
    const seen = new Set();
    suggestions = [];
    for (const raw of memberNames() || []) {
      const name = String(raw || "").toLowerCase();
      if (
        !FLOOR_NOTE_NAME_RE.test(name) ||
        name === self ||
        seen.has(name) ||
        !name.startsWith(mention.query)
      ) {
        continue;
      }
      seen.add(name);
      suggestions.push(name);
      if (suggestions.length >= FLOOR_NOTE_SUGGESTIONS) break;
    }
    if (!suggestions.length) {
      hideSuggestions();
      return;
    }
    suggestionIndex = Math.min(suggestionIndex, suggestions.length - 1);
    suggestList.innerHTML = suggestions
      .map(
        (name, index) =>
          `<li><button type="button" class="${index === suggestionIndex ? "on" : ""}" data-floor-note-mention="${escapeHTML(name)}">@${escapeHTML(name)}</button></li>`,
      )
      .join("");
    suggestList.hidden = false;
  }

  function insertMention(name) {
    const caret = input.selectionStart ?? input.value.length;
    const mention = mentionQueryAt(input.value, caret);
    if (!mention) return;
    const before = input.value.slice(0, mention.start);
    const after = input.value.slice(caret);
    const inserted = `@${name} `;
    input.value = `${before}${inserted}${after}`.slice(0, FLOOR_NOTE_TEXT_MAX);
    const position = Math.min(input.value.length, before.length + inserted.length);
    input.setSelectionRange(position, position);
    input.focus();
    hideSuggestions();
    renderCount();
  }

  function handleChanged(id) {
    window.clearTimeout(refreshTimer);
    refreshTimer = window.setTimeout(() => void refresh(), 300);
    const noteId = String(id || "");
    if (noteId && noteId === openId && thread && !panel.hidden) {
      void openNote(noteId);
    }
  }

  chip.addEventListener("click", () => {
    const point = chipPoint;
    hideChip();
    compose(point);
  });
  $("[data-floor-note-close]").addEventListener("click", close);
  sendButton.addEventListener("click", () => void submit());
  panel.addEventListener("click", (event) => {
    const mention = event.target.closest("[data-floor-note-mention]");
    if (mention) {
      insertMention(mention.dataset.floorNoteMention);
      return;
    }
    const deleteButton = event.target.closest("[data-floor-note-delete]");
    if (deleteButton) void remove(deleteButton.dataset.floorNoteDelete);
  });
  panel.addEventListener("keydown", (event) => {
    event.stopPropagation();
    if (!suggestList.hidden && suggestions.length) {
      if (event.key === "ArrowDown" || event.key === "ArrowUp") {
        event.preventDefault();
        suggestionIndex =
          (suggestionIndex + (event.key === "ArrowDown" ? 1 : -1) + suggestions.length) %
          suggestions.length;
        renderSuggestions();
        return;
      }
      if (event.key === "Enter" || event.key === "Tab") {
        event.preventDefault();
        insertMention(suggestions[suggestionIndex]);
        return;
      }
    }
    if (event.key === "Escape") {
      event.preventDefault();
      if (!suggestList.hidden) hideSuggestions();
      else close();
      return;
    }
    if (event.key === "Enter" && (event.metaKey || event.ctrlKey)) {
      event.preventDefault();
      void submit();
    }
  });
  panel.addEventListener("keyup", (event) => event.stopPropagation());
  input.addEventListener("input", () => {
    suggestionIndex = 0;
    renderSuggestions();
    renderCount();
  });
  input.addEventListener("click", renderSuggestions);
  canvasElement.addEventListener("dblclick", cancelPendingTap);
  canvasElement.addEventListener("pointerdown", hideChip);
  const syncTimer = window.setInterval(syncVisibilityTick, FLOOR_NOTE_SYNC_MS);
  const pollTimer = window.setInterval(() => {
    if (document.visibilityState === "visible") void refresh();
  }, FLOOR_NOTE_POLL_MS);
  void refresh();

  function dispose() {
    disposed = true;
    window.clearInterval(syncTimer);
    window.clearInterval(pollTimer);
    window.clearTimeout(pendingTap);
    window.clearTimeout(chipTimer);
    window.clearTimeout(refreshTimer);
    canvasElement.removeEventListener("dblclick", cancelPendingTap);
    canvasElement.removeEventListener("pointerdown", hideChip);
    for (const id of [...sprites.keys()]) removeSprite(id);
    world.scene?.remove(group);
    chip.remove();
    panel.remove();
  }

  return {
    tap,
    refresh,
    openNote,
    handleChanged,
    close,
    dispose,
    state: () => ({
      notes: [...notes.values()].map((note) => ({ ...note })),
      visible: [...sprites.keys()],
      openId,
      composing: Boolean(composing),
      chipVisible: !chip.hidden,
    }),
  };
}

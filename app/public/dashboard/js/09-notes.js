  // Collaborative Markdown notes. Edits autosave through the version-checked
  // PATCH; the socket only announces persisted changes, in-progress drafts,
  // and lightweight presence.
  const NOTE_AUTOSAVE_MS = 1500;
  const NOTE_PRESENCE_MS = 15000;
  const NOTE_EDITING_MS = 10000;
  const NOTE_COMMENTS_REFRESH_MS = 300;
  const NOTE_COMMENT_CONTEXT = 32;
  const NOTE_QUOTE_MAX = 1000;
  const NOTE_ASSIST_MAX = 6000;
  const NOTE_ASSIST_CONTEXT = 600;
  const NOTE_ASSIST_ACTIONS = [
    ["clean-up", "Clean up"], ["grammar", "Check grammar"], ["clearer", "Make clearer"],
    ["shorter", "Make shorter"], ["longer", "Make longer"], ["professional", "Professional tone"],
    ["friendly", "Friendly tone"], ["bullets", "Bullet list"], ["summarize", "Summarize"],
  ];
  const NOTE_MODE_KEY = "forkmesh.notes.layout";
  const NOTE_REACTIONS = ["👍", "❤️", "😂", "🎉", "😮", "👀"];
  const NOTE_MODES = ["write", "split", "preview", "live", "pages"];
  const NOTE_IMAGE_SETTINGS_LINE = /^\s*<!--\s*image:([\s\S]*?)-->\s*$/i;
  const NOTE_IMAGE_DEFAULTS = {
    wrap: "inline", align: "left", width: 0, height: 0, rotate: 0, margin: 3.2, x: 0, y: 0, pages: "anchor",
    recolor: "none", transparency: 0, brightness: 0, contrast: 0,
  };
  const NOTE_IMAGE_RULES = {
    wrap: { choices: ["inline", "wrap", "break", "behind", "front"] }, align: { choices: ["left", "center", "right"] },
    width: { min: 0, max: 2000 }, height: { min: 0, max: 2000 }, rotate: { min: -360, max: 360 }, margin: { min: 0, max: 100 },
    x: { min: -2000, max: 2000 }, y: { min: -2000, max: 2000 }, pages: { choices: ["anchor", "all", "first", "rest"] },
    recolor: { choices: ["none", "grayscale", "sepia", "negative"] }, transparency: { min: 0, max: 100 },
    brightness: { min: -100, max: 100 }, contrast: { min: -100, max: 100 },
  };
  const NOTE_CHAT_SEEN_KEY = "forkmesh.notes.chat-seen";
  const NOTE_SEARCH_MS = 250;
  const NOTE_ACTIVITY_KEY = "forkmesh.notes.activity";
  const NOTE_SIDE_TAB_KEY = "forkmesh.notes.sideTab";
  const NOTE_OUTLINE_KEY = "forkmesh.notes.outline";
  const NOTE_MARKDOWN_MAX = 512 * 1024;
  const NOTE_IMAGE_MAX_CHARS = 360 * 1024;
  const NOTE_IMAGE_MAX_WIDTH = 1600;
  const NOTE_SIDE_TABS = ["comments", "chat", "activity", "history"];
  const NOTE_ACTIVITY_REFRESH_MS = 1200;
  const NOTE_EDIT_GROUP_MS = 30 * 60 * 1000;
  const NOTE_ACTIVITY_OVERLAY = "(max-width: 1279px)";
  const NOTE_HISTORY_KINDS = ["edit", "create", "restore"];
  const NOTE_ACTIVITY_ICONS = {
    view: "eye", web: "globe", edit: "pencil", create: "file-plus", restore: "rotate-ccw",
    share: "user-plus", role: "user-cog", unshare: "user-minus", publish: "globe", unpublish: "eye-off",
    link: "link", unlink: "unlink", export: "download", "copy-link": "copy", task: "list-todo", issue: "circle-dot",
    comment: "message-square", resolve: "circle-check", reopen: "undo-2",
  };
  const NOTE_TASK_DETAILS_MAX = 4000;
  const NOTE_ISSUE_BODY_MAX = 60000;
  const NOTE_TASK_ERRORS = {
    not_found: "That was not found. It may have been deleted.",
    forbidden: "You do not have access to that.",
    org_member_required: "You do not have access to that.",
    unauthorized: "Sign in again to continue.",
    invalid_session: "Sign in again to continue.",
  };
  const NOTE_AVATAR_COLORS = ["#2f81f7", "#bf4b8a", "#1a7f37", "#9a6700", "#6e40c9", "#cf222e", "#0969da", "#8250df"];
  const NOTE_ERRORS = {
    principal_not_found: "No user or organization has that name.",
    invalid_principal: "Enter a valid username or organization name.",
    repository_not_found: "That repository doesn't exist, or you don't have access to it.",
    invalid_link: "Paste an issue or pull request URL, or type owner/repo#123.",
    owner_required: "Only the note's owner can do that.",
    editor_required: "You need edit access to do that.",
    note_not_found: "This note no longer exists, or you no longer have access to it.",
    page_not_found: "This page no longer exists, or you no longer have access to it.",
    invalid_page: "Something on this page isn't valid. Check the title, description and outline.",
    too_many_entries: "A page can hold up to 300 entries.",
    invalid_emoji: "That isn't an emoji. Type or paste a single emoji.",
    version_conflict: "Someone saved a newer version. Load the latest and try again.",
    assist_unavailable: "Writing with AI isn't set up on this site yet. An administrator turns it on in Settings, AI review.",
    assist_daily_limit: "You've used today's AI writing requests. Try again tomorrow.",
    assist_failed: "The AI didn't send back any text. Try again.",
    text_too_long: "Select up to 6,000 characters to write with AI.",
    prompt_required: "Type what you want the AI to do.",
    image_too_large: "That image is too large to paste into the note, even made smaller.",
    note_full: "This note has no room left for another image.",
    image_unreadable: "That image couldn't be read. Try copying it again, or as PNG.",
    invalid_reaction: "Pick an emoji to react with.",
    too_many_reactions: "This note has too many reactions. Remove some first.",
    reaction_not_found: "That reaction was already removed.",
    reaction_author_required: "You can only remove your own reactions.",
    get upstream_unavailable() { return `${platformName()} is restarting. Try again in a few seconds.`; },
    get sealed() { return `${platformName()} is restarting. Try again in a few seconds.`; },
  };
  const NOTE_READ_RETRY_DELAYS_MS = [500, 1500, 4000];
  const NOTE_PAGE_ACCENTS = {
    blue: "rgb(var(--dashboard-primary-rgb))", purple: "#8250df", pink: "#bf3989", red: "#cf222e",
    orange: "#d1600f", yellow: "#bf8700", teal: "#1b8a8f", gray: "#6e7781",
  };
  const NOTE_PAGE_MAX_ENTRIES = 300;
  const NOTE_PAGE_MAX_DEPTH = 3;
  const NOTE_PAGE_MOBILE = "(max-width: 767px)";
  const NOTE_EMOJI_CHOICES = [
    "📝", "📌", "📎", "📚", "📖", "📒", "📓", "📔", "📁", "📂", "📅", "📆", "✅", "🎯", "🚀", "💡",
    "🔥", "⭐", "✨", "🎉", "💜", "💙", "💬", "🔒", "🔑", "🔧", "🔨", "🧪", "🐛", "📈", "📊", "💰",
    "🏠", "🌍", "🌱", "☕", "🍕", "🎵", "🎨", "📷", "🏆", "🤝", "👋", "👀", "🙂", "😀", "🤔", "🧠",
  ];
  const NOTE_KEYCAP = String.fromCodePoint(0x20E3);
  const NOTE_VS16 = String.fromCodePoint(0xFE0F);

  async function noteRequest(path, method = "GET", body = null) {
    const headers = { accept: "application/json" };
    const token = state.session?.sessionToken || "";
    if (token) headers.authorization = `Bearer ${token}`;
    if (body !== null) headers["content-type"] = "application/json";
    const retries = method === "GET" ? NOTE_READ_RETRY_DELAYS_MS : [];
    let response;
    let payload;
    for (let attempt = 0; ; attempt += 1) {
      const last = attempt >= retries.length;
      try {
        response = await fetch(path, {
          method, headers, cache: "no-store",
          body: body === null ? undefined : JSON.stringify(body),
        });
      } catch (cause) {
        if (last) throw cause;
        response = null;
      }
      if (response) {
        payload = await response.json().catch(() => ({}));
        if (response.ok || last || (response.status < 500 && response.status !== 408)) break;
      }
      await new Promise((resolve) => setTimeout(resolve, retries[attempt]));
    }
    if (!response.ok) {
      const error = new Error(NOTE_ERRORS[payload.error] || payload.error || `HTTP ${response.status}`);
      error.status = response.status; error.payload = payload; throw error;
    }
    return payload;
  }

  function notesError(message = "") {
    const box = $("[data-notes-error]");
    if (!box) return;
    box.replaceChildren();
    if (message) {
      box.innerHTML = `<i data-lucide="circle-alert"></i><div class="fm-flash-body">${escapeHtml(message)}</div>`;
      refreshNoteIcons();
    }
    box.hidden = !message;
  }

  function noteNotice(place, message = "", tone = "good") {
    document.querySelectorAll("[data-note-notice]").forEach((node) => {
      const shown = Boolean(message) && node.dataset.noteNotice === place;
      node.textContent = shown ? message : "";
      node.dataset.tone = tone;
      node.hidden = !shown;
    });
  }

  function refreshNoteIcons() { window.lucide?.createIcons(); }

  function noteIcon(name) { return `<i data-lucide="${name}"></i>`; }

  function noteCanEdit(note) { return ["owner", "editor"].includes(note?.role); }

  function noteAccessUrl(note) { return `${location.origin}/dashboard/notes?note=${encodeURIComponent(note.id)}`; }

  function noteLinkedId() {
    const id = new URLSearchParams(location.search).get("note") || "";
    return /^[0-9a-f]{32}$/.test(id) ? id : "";
  }

  function notePageLinkedId() {
    const id = new URLSearchParams(location.search).get("page") || "";
    return /^[0-9a-f]{32}$/.test(id) ? id : "";
  }

  function noteLinkedComment() {
    const id = new URLSearchParams(location.search).get("comment") || "";
    return /^[0-9a-f]{32}$/.test(id) ? id : "";
  }

  function rememberNoteInUrl(note) {
    if (!location.pathname.startsWith("/dashboard/notes")) return;
    const url = new URL(location.href);
    if (url.searchParams.get("note") === note.id && !url.searchParams.has("page")) return;
    url.searchParams.delete("page");
    url.searchParams.set("note", note.id);
    history.replaceState(history.state, "", url.pathname + url.search + url.hash);
  }

  function rememberNotePageInUrl(pageId, noteId = "") {
    if (!location.pathname.startsWith("/dashboard/notes")) return;
    const url = new URL(location.href);
    url.searchParams.delete("page"); url.searchParams.delete("note"); url.searchParams.delete("comment");
    url.searchParams.set("page", pageId);
    if (noteId) url.searchParams.set("note", noteId);
    history.replaceState(history.state, "", url.pathname + url.search + url.hash);
  }

  function noteEmojiValid(value) {
    const text = String(value || "");
    const points = [...text];
    if (!points.length || points.length > 16 || new TextEncoder().encode(text).length > 64) return false;
    return points.every((char, index) => {
      if (/[\s\p{Cc}]/u.test(char)) return false;
      if (/[0-9#*]/.test(char)) {
        return points[index + 1] === NOTE_KEYCAP || (points[index + 1] === NOTE_VS16 && points[index + 2] === NOTE_KEYCAP);
      }
      if (char.codePointAt(0) < 128) return false;
      return /[\p{S}\p{M}\p{Cf}]/u.test(char);
    });
  }

  function noteEmojiHtml(emoji, className = "notes-emoji") {
    return emoji ? `<span class="${className}">${escapeHtml(emoji)}</span>` : "";
  }

  async function copyNoteLink(url, place, failure) {
    try {
      await navigator.clipboard.writeText(url);
      noteNotice(place, "Link copied");
      void recordNoteActivity("copy-link");
    } catch (_) { noteNotice(place, failure, "bad"); }
  }

  function noteInitials(name) {
    const parts = String(name || "?").replace(/[^\p{L}\p{N}\s_-]/gu, "").split(/[\s_-]+/).filter(Boolean);
    return ((parts[0]?.[0] || "?") + (parts[1]?.[0] || "")).slice(0, 2);
  }

  function noteAvatarTagHtml(name, className, attrs, inner) {
    const href = issuePersonProfileHref(String(name || "").trim().toLowerCase());
    return href
      ? `<a href="${escapeHtml(href)}" class="${className}" ${attrs} aria-label="@${escapeHtml(name)} profile">${inner}</a>`
      : `<span class="${className}" ${attrs}>${inner}</span>`;
  }

  function noteAvatarColor(name) {
    let hash = 0;
    for (const char of String(name || "")) hash = (hash * 31 + char.codePointAt(0)) >>> 0;
    return NOTE_AVATAR_COLORS[hash % NOTE_AVATAR_COLORS.length];
  }

  // "Public" / "Shared" / "Private" — the one word that answers "can anyone
  // else see this?", which is the question the sidebar exists to answer.
  function noteStatusLabel(note) {
    if (note.visibility === "public") return "Public";
    return (note.shares || []).length ? "Shared" : "Private";
  }

  // "12 views" / "12 views from 4 readers", or "" for a note nobody has read
  // — an unpublished draft should not be labelled "0 views".
  function noteViewsLabel(note) {
    const views = Number(note.views || 0), readers = Number(note.readers || 0);
    if (views <= 0) return "";
    const counted = views === 1 ? "1 view" : `${views} views`;
    return readers > 1 ? `${counted} from ${readers} readers` : counted;
  }

  function noteSharedWithLabel(note) {
    const names = (note.shares || [])
      .filter((share) => share.name)
      .map((share) => `${share.name} (${share.role || "viewer"})`);
    return names.length ? `Shared with ${names.join(", ")}` : "Not shared with anyone";
  }

  function notePlural(value, one, many) { return Number(value || 0) === 1 ? one : many; }

  function noteCountLabel(value, one, many) { return `${Number(value || 0)} ${notePlural(value, one, many)}`; }

  function noteStatsHtml(note) {
    return [
      ["eye", note.views, noteViewsLabel(note) || "No views yet"],
      ["history", note.version, noteCountLabel(note.version, "version", "versions")],
      ["activity", note.actions, noteCountLabel(note.actions, "action", "actions")],
    ].map(([icon, value, label]) =>
      `<span class="notes-row-stat" title="${escapeHtml(label)}" aria-label="${escapeHtml(label)}">${noteIcon(icon)}${Number(value || 0)}</span>`).join("");
  }

  function noteVisibilityIcon(note) {
    const status = noteStatusLabel(note);
    return status === "Public" ? "globe" : status === "Shared" ? "users" : "lock";
  }

  function noteMatchesFilter(note, filter) {
    if (filter === "mine") return note.role === "owner";
    if (filter === "shared") return note.role !== "owner";
    if (filter === "public") return note.visibility === "public";
    return true;
  }

  function noteEditedWhen(value, now = new Date()) {
    const at = new Date(typeof value === "number" ? value : Date.parse(value || "") || 0);
    if (!at.getTime()) return "";
    const time = at.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
    const days = Math.round((new Date(now.getFullYear(), now.getMonth(), now.getDate())
      - new Date(at.getFullYear(), at.getMonth(), at.getDate())) / 86400000);
    if (days === 0) return `Today ${time}`;
    if (days === 1) return `Yesterday ${time}`;
    if (at.getFullYear() === now.getFullYear()) return `${at.toLocaleDateString([], { month: "short", day: "numeric" })}, ${time}`;
    return at.toLocaleDateString([], { month: "short", day: "numeric", year: "numeric" });
  }

  function noteEditedLabel(note) {
    const when = noteEditedWhen(note.updatedAt);
    if (!when) return "";
    const by = String(note.lastEditedBy || "");
    return `Edited ${when}${by && by !== noteMe() ? ` · by ${by}` : ""}`;
  }

  function noteSharePeopleHtml(note) {
    const shares = (note.shares || []).filter((share) => share.name);
    if (!shares.length) return "";
    return `<span class="notes-row-people" title="${escapeHtml(noteSharedWithLabel(note))}">${shares.slice(0, 3).map((share) =>
      `<span class="notes-avatar" data-type="${share.type === "organization" ? "organization" : "user"}" style="background:${noteAvatarColor(share.name)}">${escapeHtml(noteInitials(share.name))}</span>`).join("")}${shares.length > 3 ? `<span class="notes-avatar notes-row-more">+${shares.length - 3}</span>` : ""}</span>`;
  }

  function markNoteSeen(id) {
    const view = state.notesView;
    view.items = view.items.map((item) => item.id === id ? { ...item, unread: false, unreadComments: 0 } : item);
    if (view.selected?.id === id) view.selected = { ...view.selected, unread: false, unreadComments: 0 };
    renderNoteList();
  }

  function noteStillWatching() {
    const view = state.notesView;
    if (!view.selected || Date.now() - (view.seenAt || 0) < 5000) return;
    view.seenAt = Date.now();
    void recordNoteActivity("view");
  }

  async function refreshNoteList() {
    const view = state.notesView;
    if (!state.session?.sessionToken || document.hidden) return;
    try {
      const payload = await noteRequest("/api/notes");
      const fresh = new Map((payload.notes || []).map((note) => [note.id, note]));
      view.items = (payload.notes || []).map((note) => {
        const known = view.items.find((item) => item.id === note.id);
        const merged = { ...known, ...note };
        return note.id === view.selected?.id ? { ...merged, unread: false, unreadComments: 0 } : merged;
      });
      if (view.selected && !fresh.has(view.selected.id)) return;
      renderNoteList();
    } catch (_) {}
    void loadNotePages();
  }

  function renderNoteRow(note) {
    const isSelected = state.notesView.selected?.id === note.id;
    const status = noteStatusLabel(note);
    const unread = Boolean(note.unread) && !isSelected;
    const edited = noteEditedLabel(note);
    const updated = note.updatedAt ? new Date(typeof note.updatedAt === "number" ? note.updatedAt : Date.parse(note.updatedAt)).toLocaleString() : "";
    const comments = Number(note.openComments || 0);
    // Owners see who else can read the note; collaborators see what they
    // are allowed to do with someone else's note.
    const access = note.role === "owner"
      ? noteSharedWithLabel(note)
      : `You can ${note.role === "editor" ? "edit" : "view"}`;
    return `
      <button type="button" data-note-id="${escapeHtml(note.id)}" aria-selected="${isSelected ? "true" : "false"}" data-unread="${unread ? "true" : "false"}"${(note.shares || []).length ? " data-people" : ""} class="notes-row">
        <span class="notes-row-head">${unread ? `<span class="notes-row-dot" title="${escapeHtml(Number(note.unreadComments || 0) ? `Changed since you last opened it · ${noteCountLabel(note.unreadComments, "new comment", "new comments")}` : "Changed since you last opened it")}" aria-label="Unread"></span>` : ""}${noteEmojiHtml(note.emoji, "notes-row-emoji")}<span class="notes-row-title">${escapeHtml(note.title || "Untitled note")}</span></span>
        <span class="notes-row-vis" data-visibility="${note.visibility === "public" ? "public" : "private"}" title="${escapeHtml(status)}" aria-label="${escapeHtml(status)}">${noteIcon(noteVisibilityIcon(note))}${escapeHtml(status)}</span>
        <span class="notes-row-line" title="${escapeHtml(access)}">${escapeHtml(access)}</span>
        ${noteSharePeopleHtml(note)}
        <span class="notes-row-meta"><span class="notes-row-when" title="${escapeHtml(updated)}">${escapeHtml(edited || relativeTimeLabel(note.updatedAt))}</span>${comments ? `<span class="notes-row-stat" title="${escapeHtml(noteCountLabel(comments, "open comment", "open comments"))}" aria-label="${escapeHtml(noteCountLabel(comments, "open comment", "open comments"))}">${noteIcon("message-square")}${comments}</span>` : ""}${noteStatsHtml(note)}</span>
      </button>`;
  }

  function renderNoteList() {
    const list = $("[data-note-list]"); if (!list) return;
    const view = state.notesView;
    const query = noteSearchQuery();
    const matches = view.searchMatches?.query === query ? view.searchMatches.ids : null;
    const notes = view.items.filter((note) => noteMatchesFilter(note, view.filter)
      && (String(note.title || "").toLowerCase().includes(query)
        || String(note.markdown || "").toLowerCase().includes(query)
        || Boolean(matches?.has(note.id))));
    if (!notes.length) {
      const empty = query && !matches ? "Searching notes…"
        : query ? `No notes match “${escapeHtml(query)}”.`
        : view.loading ? "Loading notes…"
        : view.filter === "shared" ? "Nobody has shared a note with you yet."
        : view.filter === "public" ? "You haven't published any notes."
        : "No notes yet. Create one to get started.";
      list.innerHTML = `<p class="px-3 py-6 text-center text-xs text-muted-foreground">${empty}</p>`;
      return;
    }
    if (view.filter !== "all") {
      list.innerHTML = notes.map(renderNoteRow).join("");
    } else {
      const groups = [
        ["Your notes", notes.filter((note) => note.role === "owner")],
        ["Shared with you", notes.filter((note) => note.role !== "owner")],
      ].filter(([, rows]) => rows.length);
      list.innerHTML = groups.map(([label, rows]) =>
        (groups.length > 1 || view.pages?.length ? `<div class="notes-group">${label}</div>` : "")
        + rows.map(renderNoteRow).join("")).join("");
    }
    refreshNoteIcons();
  }

  function noteSearchQuery() {
    return String($("[data-note-search]")?.value || "").trim().toLowerCase().slice(0, 200);
  }

  function searchNotes() {
    const view = state.notesView, query = noteSearchQuery();
    const generation = ++view.searchGeneration;
    clearTimeout(view.searchTimer);
    renderNotePages();
    if (!query) { view.searchMatches = null; renderNoteList(); return; }
    renderNoteList();
    view.searchTimer = setTimeout(async () => {
      let ids = new Set();
      try {
        const payload = await noteRequest(`/api/notes?query=${encodeURIComponent(query)}`);
        ids = new Set((payload.notes || []).map((note) => note.id));
      } catch (_) {}
      if (generation !== view.searchGeneration) return;
      view.searchMatches = { query, ids };
      renderNoteList();
    }, NOTE_SEARCH_MS);
  }

  function setNoteSaveState(kind, detail = "") {
    const badge = $("[data-note-save-state]"); if (!badge) return;
    badge.dataset.state = kind;
    if (kind === "error") {
      badge.innerHTML = `Not saved <button type="button" data-note-save-retry>Retry</button>`;
      badge.title = detail;
      return;
    }
    badge.title = "";
    badge.textContent = kind === "saving" ? "Saving…"
      : kind === "dirty" ? (detail || "Unsaved changes")
      : kind === "readonly" ? "View only"
      : detail || "Saved";
  }

  function renderNotePreview() {
    const title = $("[data-note-title]")?.value || "Untitled note";
    const markdown = $("[data-note-markdown]")?.value || "";
    const emoji = state.notesView.selected?.emoji || "";
    const heading = $("[data-note-preview-title]");
    if (heading && document.activeElement !== heading) heading.textContent = noteLiveMode() ? $("[data-note-title]")?.value || "" : title;
    const crumb = $("[data-note-crumb]");
    if (crumb) crumb.textContent = title;
    document.querySelectorAll("[data-note-preview-emoji], [data-note-crumb-emoji]").forEach((node) => {
      node.textContent = emoji; node.hidden = !emoji;
    });
    const preview = $("[data-note-preview]");
    if (preview) {
      if (noteLiveMode()) {
        renderNoteLive(preview, markdown);
        unmarkNoteComments(preview);
        preview.querySelectorAll(".notes-reactions").forEach((row) => row.remove());
      } else {
        if (state.notesView.live) state.notesView.live = null;
        preview.innerHTML = markdown.trim()
          ? renderMarkdown(markdown, { noteDocument: true })
          : '<p class="notes-preview-empty">Nothing written yet.</p>';
      }
      const lines = noteLines(markdown);
      if (noteLiveMode()) {
        for (const block of preview.querySelectorAll(":scope > .notes-live-block:not([data-editing=\"true\"])")) decorateNoteImagesIn(block, lines, Number(block.dataset.liveLine) || 0);
      } else decorateNoteImagesIn(preview, lines);
      if (state.notesView.imageSelection?.where === "live") requestAnimationFrame(reselectNoteImage);
      if (notePagesMode()) scheduleNotePages();
      markNoteCommentsInPreview(preview);
      renderNoteReactions(preview, markdown);
      renderNotePeerMarks();
    }
    forgetNoteScrollGeometry();
    renderNoteOutline(markdown);
    const view = state.notesView;
    if ((view.mode === "split" || view.outlineOpen) && !view.scrollFrame) {
      view.scrollFrame = requestAnimationFrame(() => { view.scrollFrame = 0; syncNoteScroll(view.scrollLead); renderNoteOutlineCurrent(); });
    }
    const words = markdown.split(/\s+/).filter(Boolean).length;
    const counts = $("[data-note-counts]");
    if (counts) counts.textContent = `${words} ${words === 1 ? "word" : "words"} · ${Math.max(1, Math.round(words / 220))} min read · Markdown`;
  }

  const NOTE_LAYOUT_DEFAULTS = {
    size: "a4", orientation: "portrait", margins: "normal", cover: "no", contents: "no", numbers: "yes",
  };
  const NOTE_LAYOUT_CHOICES = {
    size: ["a4", "letter", "legal"], orientation: ["portrait", "landscape"], margins: ["narrow", "normal", "wide"],
    cover: ["yes", "no"], contents: ["yes", "no"], numbers: ["yes", "no"],
  };
  const NOTE_PAPER_MM = { a4: [210, 297], letter: [215.9, 279.4], legal: [215.9, 355.6] };
  const NOTE_MARGIN_MM = { narrow: 12.7, normal: 25.4, wide: 38.1 };

  function noteLayoutLineIndex(lines) {
    let fence = "";
    for (let i = 0; i < lines.length; i++) {
      const marker = /^\s*(```|~~~)/.exec(lines[i]);
      if (fence) {
        if (marker && marker[1] === fence) fence = "";
      } else if (marker) {
        fence = marker[1];
      } else if (NOTE_LAYOUT_LINE.test(lines[i])) {
        return i;
      }
    }
    return -1;
  }

  function noteLines(markdown) {
    return String(markdown || "").replace(/\r\n?/g, "\n").split("\n");
  }

  function noteLayoutRead(markdown) {
    const layout = { ...NOTE_LAYOUT_DEFAULTS };
    const lines = noteLines(markdown);
    const at = noteLayoutLineIndex(lines);
    if (at < 0) return layout;
    for (const pair of NOTE_LAYOUT_LINE.exec(lines[at])[1].split(";")) {
      const [key, value] = pair.split("=").map((part) => String(part || "").trim().toLowerCase());
      if (Object.hasOwn(NOTE_LAYOUT_CHOICES, key) && NOTE_LAYOUT_CHOICES[key].includes(value)) layout[key] = value;
    }
    return layout;
  }

  function noteLayoutLine(layout) {
    return `<!-- page: ${Object.keys(NOTE_LAYOUT_DEFAULTS).map((key) =>
      `${key}=${NOTE_LAYOUT_CHOICES[key].includes(layout?.[key]) ? layout[key] : NOTE_LAYOUT_DEFAULTS[key]}`).join("; ")} -->`;
  }

  function noteWithLayout(markdown, layout) {
    const lines = noteLines(markdown);
    const at = noteLayoutLineIndex(lines);
    if (at >= 0) lines.splice(at, 1);
    while (lines.length && !lines[0].trim()) lines.shift();
    return `${noteLayoutLine(layout)}\n\n${lines.join("\n")}`;
  }

  function notePdfFileName(title) {
    return `${String(title || "").replace(/[^\w.-]+/g, "-").replace(/^-+|-+$/g, "").toLowerCase() || "note"}.pdf`;
  }

  function notePrintStyles(layout) {
    const [width, height] = NOTE_PAPER_MM[layout.size];
    const [pageWidth, pageHeight] = layout.orientation === "landscape" ? [height, width] : [width, height];
    const margin = NOTE_MARGIN_MM[layout.margins];
    const numbers = layout.numbers === "yes"
      ? '@bottom-center { content: counter(page) " / " counter(pages); font: 9pt -apple-system, "Segoe UI", system-ui, Roboto, Arial, sans-serif; color: #57606a; }'
      : "";
    const coverPage = layout.cover === "yes" && numbers ? "@page :first { @bottom-center { content: none; } }" : "";
    const mono = 'ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas, monospace';
    return `@page { size: ${pageWidth}mm ${pageHeight}mm; margin: ${margin}mm; ${numbers} }
${coverPage}
html { print-color-adjust: exact; -webkit-print-color-adjust: exact; }
body { margin: 0; background: #fff; color: #1f2328; font: 11pt/1.45 -apple-system, "Segoe UI", system-ui, Roboto, "Helvetica Neue", Arial, sans-serif; overflow-wrap: break-word; }
h1, h2, h3, h4, h5, h6 { margin: 16pt 0 6pt; line-height: 1.25; font-weight: 600; break-after: avoid; page-break-after: avoid; }
h1 { font-size: 20pt; } h2 { font-size: 15pt; } h3 { font-size: 12.5pt; } h4 { font-size: 11pt; } h5, h6 { font-size: 10pt; }
body > article > :first-child { margin-top: 0; }
p { margin: 0 0 8pt; }
a { color: #0969da; text-decoration: none; }
ul, ol { margin: 0 0 8pt; padding-left: 18pt; }
li { margin: 2pt 0; }
.list-none { list-style: none; }
.-ml-5 { margin-left: -18pt; }
.flex { display: flex; align-items: flex-start; gap: 6pt; }
.text-center { text-align: center; }
pre { margin: 0 0 10pt; padding: 8pt; border: .5pt solid #d0d7de; border-radius: 4pt; background: #f6f8fa; font: 9pt/1.4 ${mono}; white-space: pre-wrap; break-inside: avoid; }
code { border-radius: 2pt; background: #f6f8fa; padding: 0 2pt; font: 9pt ${mono}; }
pre code { background: none; padding: 0; }
blockquote { margin: 0 0 8pt; border-left: 2pt solid #d0d7de; padding-left: 10pt; color: #57606a; }
table { width: 100%; margin: 0 0 10pt; border-collapse: collapse; font-size: 9.5pt; }
th, td { border: .5pt solid #d0d7de; padding: 3pt 5pt; text-align: left; vertical-align: top; }
th { background: #f6f8fa; }
tr, img { break-inside: avoid; }
img { max-width: 100%; }
hr { margin: 12pt 0; border: 0; border-top: .5pt solid #d0d7de; }
.note-figure { margin: 10pt 0 12pt; text-align: center; break-inside: avoid; }
.note-figure img { display: block; max-width: 100%; height: auto; margin: 0 auto; }
.note-figure figcaption { margin-top: 4pt; font-size: 9pt; color: #57606a; }
.note-page-break { break-after: page; page-break-after: always; height: 0; margin: 0; border: 0; }
.note-page-break span { display: none; }
.note-cover { break-after: page; page-break-after: always; padding-top: ${Math.round((pageHeight - 2 * margin) * 0.22)}mm; text-align: center; }
.note-cover-emoji { font-size: 48pt; line-height: 1.2; }
.note-cover h1 { margin: 10pt 0 8pt; font-size: 28pt; }
.note-cover-date { font-size: 12pt; color: #57606a; }
.note-contents { break-after: page; page-break-after: always; }
.note-contents ol { list-style: none; margin: 0; padding: 0; }
.note-contents li { margin: 0 0 5pt; }
.note-contents .note-contents-2 { padding-left: 16pt; font-size: 10pt; }
.note-image[data-wrap="wrap"] { float: left; margin: 2pt var(--note-image-margin) var(--note-image-margin) 0; }
.note-image[data-wrap="wrap"][data-align="right"] { float: right; margin: 2pt 0 var(--note-image-margin) var(--note-image-margin); }
.note-image[data-wrap="break"] { display: block; }
.note-image[data-wrap="break"][data-align="center"] { margin-left: auto; margin-right: auto; }
.note-image[data-wrap="break"][data-align="right"] { margin-left: auto; }`;
  }

  function notePrintDocument({ title, emoji = "", markdown, layout, date = new Date() }) {
    const heading = String(title || "").trim() || "Untitled note";
    let body = renderMarkdown(markdown, { noteDocument: true }).replace(/ loading="lazy"/g, "");
    const contents = [];
    body = body.replace(/<h([1-6]) data-repo-markdown-heading="([^"]*)"/g, (whole, level, slug) => `<h${level} id="note-${slug}" data-repo-markdown-heading="${slug}"`);
    for (const match of body.matchAll(/<h([12]) id="(note-[^"]*)"[^>]*>([\s\S]*?)<\/h\1>/g)) {
      contents.push(`<li class="note-contents-${match[1]}"><a href="#${match[2]}">${match[3].replace(/<[^>]*>/g, "")}</a></li>`);
    }
    const when = new Intl.DateTimeFormat("en-US", { month: "long", day: "numeric", year: "numeric" }).format(date);
    const cover = layout.cover === "yes"
      ? `<section class="note-cover">${emoji ? `<div class="note-cover-emoji">${escapeHtml(emoji)}</div>` : ""}<h1>${escapeHtml(heading)}</h1><div class="note-cover-date">${escapeHtml(when)}</div></section>`
      : "";
    const list = layout.contents === "yes"
      ? `<section class="note-contents"><h1>Contents</h1><ol>${contents.join("")}</ol></section>`
      : "";
    const lead = layout.cover === "yes" ? "" : `<h1>${escapeHtml(heading)}</h1>`;
    return `<!doctype html><html lang="en"><head><meta charset="utf-8"><title>${escapeHtml(notePdfFileName(title).replace(/\.pdf$/, ""))}</title><style>${notePrintStyles(layout)}</style></head><body>${cover}${list}<article>${lead}${body}</article></body></html>`;
  }

  function printNoteDocument(html, layout = null, markdown = "") {
    document.querySelector("[data-note-print-frame]")?.remove();
    const frame = document.createElement("iframe");
    frame.dataset.notePrintFrame = "";
    frame.setAttribute("sandbox", "allow-same-origin allow-modals");
    frame.setAttribute("aria-hidden", "true");
    frame.tabIndex = -1;
    frame.style.cssText = "position:fixed;right:0;bottom:0;width:1px;height:1px;border:0;opacity:0;pointer-events:none";
    frame.addEventListener("load", async () => {
      const images = [...(frame.contentDocument?.images || [])];
      await Promise.all(images.map((image) => image.decode().catch(() => null)));
      if (layout && frame.contentDocument) paginateNoteDocument(frame.contentDocument, layout, noteLines(markdown));
      frame.contentWindow?.focus();
      frame.contentWindow?.print();
    }, { once: true });
    frame.srcdoc = html;
    document.body.append(frame);
    return frame;
  }

  function openNotePdfDialog() {
    closeNotePopovers();
    const note = state.notesView.selected; if (!note) return;
    const area = $("[data-note-markdown]");
    const layout = noteLayoutRead(area?.value || "");
    document.querySelectorAll("[data-note-pdf-field]").forEach((field) => {
      const key = field.dataset.notePdfField;
      if (field.type === "checkbox") field.checked = layout[key] === "yes";
      else field.value = layout[key];
    });
    const canSave = noteCanEdit(note) && !area?.readOnly;
    $("[data-note-pdf-save-row]").hidden = !canSave;
    $("[data-note-pdf-save]").checked = canSave;
    $("[data-note-pdf-dialog]").showModal();
  }

  function exportNotePdf() {
    const layout = {};
    document.querySelectorAll("[data-note-pdf-field]").forEach((field) => {
      const key = field.dataset.notePdfField;
      layout[key] = field.type === "checkbox" ? (field.checked ? "yes" : "no") : field.value;
    });
    $("[data-note-pdf-dialog]").close();
    const area = $("[data-note-markdown]");
    if (!$("[data-note-pdf-save-row]").hidden && $("[data-note-pdf-save]").checked) {
      const next = noteWithLayout(area.value, layout);
      if (next !== area.value) {
        area.value = next;
        area.dispatchEvent(new Event("input", { bubbles: true }));
      }
    }
    const resolved = noteLayoutRead(noteLayoutLine(layout));
    printNoteDocument(notePrintDocument({
      title: $("[data-note-title]")?.value || "",
      emoji: state.notesView.selected?.emoji || "",
      markdown: area.value,
      layout: resolved,
    }), resolved, area.value);
  }

  function noteImageSettingsRead(text) {
    const settings = { ...NOTE_IMAGE_DEFAULTS };
    for (const pair of String(text || "").split(";")) {
      const [key, raw] = pair.split("=").map((part) => String(part || "").trim().toLowerCase());
      const rule = NOTE_IMAGE_RULES[key];
      if (!rule || raw === undefined || raw === "") continue;
      if (rule.choices) { if (rule.choices.includes(raw)) settings[key] = raw; continue; }
      const number = Number(raw);
      if (Number.isFinite(number)) settings[key] = Math.round(Math.min(rule.max, Math.max(rule.min, number)) * 10) / 10;
    }
    return settings;
  }

  function noteImageSettingsLine(settings) {
    const pairs = Object.keys(NOTE_IMAGE_DEFAULTS)
      .filter((key) => settings?.[key] !== undefined && settings[key] !== NOTE_IMAGE_DEFAULTS[key])
      .map((key) => `${key}=${settings[key]}`);
    return pairs.length ? `<!-- image: ${pairs.join("; ")} -->` : "";
  }

  function noteImageSettingsAt(lines, line) {
    const match = NOTE_IMAGE_SETTINGS_LINE.exec(lines[line - 1] || "");
    return noteImageSettingsRead(match ? match[1] : "");
  }

  function noteWithImageSettings(markdown, line, settings) {
    const lines = noteLines(markdown);
    const text = noteImageSettingsLine(settings);
    const at = line > 0 && NOTE_IMAGE_SETTINGS_LINE.test(lines[line - 1] || "") ? line - 1 : -1;
    let next = line;
    if (at >= 0 && text) lines[at] = text;
    else if (at >= 0) { lines.splice(at, 1); next = line - 1; }
    else if (text) { lines.splice(line, 0, text); next = line + 1; }
    return { markdown: lines.join("\n"), line: next };
  }

  function noteImageFilter(settings) {
    const filters = [];
    if (settings.recolor === "grayscale") filters.push("grayscale(1)");
    if (settings.recolor === "sepia") filters.push("sepia(1)");
    if (settings.recolor === "negative") filters.push("invert(1)");
    if (settings.brightness) filters.push(`brightness(${(1 + settings.brightness / 100).toFixed(2)})`);
    if (settings.contrast) filters.push(`contrast(${(1 + settings.contrast / 100).toFixed(2)})`);
    return filters.join(" ");
  }

  function styleNoteImage(image, settings) {
    image.classList.add("note-image");
    image.dataset.wrap = settings.wrap;
    image.dataset.align = settings.align;
    image.style.width = settings.width ? `${settings.width}mm` : "";
    image.style.height = settings.height ? `${settings.height}mm` : settings.width ? "auto" : "";
    image.style.maxWidth = settings.width ? "none" : "";
    image.style.transform = settings.rotate ? `rotate(${settings.rotate}deg)` : "";
    image.style.opacity = settings.transparency ? String(1 - settings.transparency / 100) : "";
    image.style.filter = noteImageFilter(settings);
    image.style.setProperty("--note-image-margin", `${settings.margin}mm`);
  }

  function noteImageLineOf(lines, paragraphLine, index) {
    let count = 0;
    for (let line = paragraphLine; line < lines.length && lines[line].trim(); line += 1) {
      const here = (lines[line].match(/!\[[^\]]*\]\(/g) || []).length;
      if (index < count + here) return index === count && /^\s*(?:\[\s*)?!\[/.test(lines[line]) ? line : -1;
      count += here;
    }
    return -1;
  }

  function decorateNoteImagesIn(root, lines, offset = 0) {
    for (const paragraph of root.querySelectorAll("p[data-note-line]")) {
      const start = Number(paragraph.dataset.noteLine) + offset;
      [...paragraph.querySelectorAll("img")].forEach((image, index) => {
        const line = noteImageLineOf(lines, start, index);
        if (line < 0) return;
        image.dataset.noteImageLine = String(line);
        image.draggable = false;
        styleNoteImage(image, noteImageSettingsAt(lines, line));
      });
    }
  }

  function notePageGeometry(layout) {
    const [width, height] = NOTE_PAPER_MM[layout.size];
    const [page, tall] = layout.orientation === "landscape" ? [height, width] : [width, height];
    const margin = NOTE_MARGIN_MM[layout.margins];
    return { width: page, height: tall, margin, contentWidth: page - 2 * margin, contentHeight: tall - 2 * margin };
  }

  function notePagesStyles(geometry, screen) {
    const { width, height, margin, contentWidth, contentHeight } = geometry;
    return `@page { size: ${width}mm ${height}mm; margin: 0; }
html, body { margin: 0; }
${screen ? "html { background: #e8eaed; } body { padding: 24px 0; }" : ""}
.np-measure { position: absolute; left: 0; top: 0; visibility: hidden; }
.np-flow { width: ${contentWidth}mm; height: ${contentHeight}mm; column-width: ${contentWidth}mm; column-gap: ${2 * margin}mm; column-fill: auto; }
.np-flow img { max-height: ${contentHeight}mm; object-fit: contain; }
.np-flow .note-cover, .np-flow .note-contents, .np-flow .note-page-break { break-after: column; }
.np-pages { width: ${width}mm; margin: 0 auto; }
.np-page { position: relative; width: ${width}mm; height: ${height}mm; overflow: hidden; background: #fff; break-after: page; page-break-after: always; }
${screen ? `.np-page { margin: 0 auto 24px; box-shadow: 0 1px 3px rgb(60 64 67 / .3), 0 4px 8px rgb(60 64 67 / .15); }` : ".np-page:last-child { break-after: auto; page-break-after: auto; }"}
.np-win { position: absolute; left: ${margin}mm; top: ${margin}mm; width: ${contentWidth}mm; height: ${contentHeight}mm; overflow: hidden; z-index: 1; }
.np-win > .np-flow { position: absolute; top: 0; }
.np-layer { position: absolute; inset: 0; overflow: hidden; pointer-events: none; }
.np-layer-behind { z-index: 0; } .np-layer-front { z-index: 2; }
.np-layer img { position: absolute; max-width: none; max-height: none; margin: 0; transform-origin: center; pointer-events: auto; }
.np-number { position: absolute; left: 0; right: 0; bottom: ${margin / 2}mm; transform: translateY(50%); z-index: 3; text-align: center; font: 9pt -apple-system, "Segoe UI", system-ui, Roboto, Arial, sans-serif; color: #57606a; }
.np-select { position: fixed; z-index: 10; border: 2px solid #1a73e8; pointer-events: none; }
.np-select span { position: absolute; right: -6px; bottom: -6px; width: 10px; height: 10px; border: 2px solid #fff; background: #1a73e8; pointer-events: auto; cursor: nwse-resize; }
${screen ? ".np-page img[data-note-image-line] { cursor: pointer; } .np-layer img[data-note-image-line] { cursor: move; }" : ""}`;
  }

  function paginateNoteDocument(doc, layout, lines, { screen = false } = {}) {
    const geometry = notePageGeometry(layout);
    const { width, height, margin } = geometry;
    const style = doc.createElement("style");
    style.textContent = notePagesStyles(geometry, screen);
    doc.head.appendChild(style);
    const measure = doc.createElement("div");
    measure.className = "np-measure";
    const flow = doc.createElement("div");
    flow.className = "np-flow";
    flow.append(...doc.body.childNodes);
    measure.appendChild(flow);
    doc.body.appendChild(measure);
    decorateNoteImagesIn(flow, lines);
    const placed = [];
    for (const image of flow.querySelectorAll("img[data-wrap=\"behind\"], img[data-wrap=\"front\"]")) {
      const anchor = doc.createElement("span");
      anchor.className = "np-anchor";
      image.replaceWith(anchor);
      placed.push({ image, anchor, settings: noteImageSettingsAt(lines, Number(image.dataset.noteImageLine)) });
    }
    const box = flow.getBoundingClientRect();
    const perMm = box.width / geometry.contentWidth, pitch = width * perMm;
    const pageAt = (x) => Math.max(0, Math.floor((x - box.left) / pitch));
    let count = 1;
    for (const element of flow.querySelectorAll("*")) {
      const rect = element.getBoundingClientRect();
      if (rect.width || rect.height) count = Math.max(count, pageAt(rect.right - 1) + 1);
    }
    const named = [...flow.querySelectorAll("[id]")].map((element) => pageAt(element.getBoundingClientRect().left + 1));
    for (const item of placed) item.page = Math.min(count - 1, pageAt(item.anchor.getBoundingClientRect().left + 1));
    const pages = doc.createElement("div");
    pages.className = "np-pages";
    for (let index = 0; index < count; index += 1) {
      const page = doc.createElement("section");
      page.className = "np-page";
      page.dataset.page = String(index + 1);
      const layers = { behind: doc.createElement("div"), front: doc.createElement("div") };
      layers.behind.className = "np-layer np-layer-behind";
      layers.front.className = "np-layer np-layer-front";
      for (const item of placed) {
        const { pages: which } = item.settings;
        const shown = which === "all" || (which === "first" && index === 0) || (which === "rest" && index > 0) || (which === "anchor" && item.page === index);
        if (!shown) continue;
        const image = item.image.cloneNode(true);
        image.style.left = `${item.settings.x}mm`;
        image.style.top = `${item.settings.y}mm`;
        layers[item.settings.wrap].appendChild(image);
      }
      const view = doc.createElement("div");
      view.className = "np-win";
      const copy = flow.cloneNode(true);
      copy.style.left = `-${index * width}mm`;
      [...copy.querySelectorAll("[id]")].forEach((element, at) => { if (named[at] !== index) element.removeAttribute("id"); });
      view.appendChild(copy);
      page.append(layers.behind, view, layers.front);
      if (layout.numbers === "yes" && !(layout.cover === "yes" && index === 0)) {
        const number = doc.createElement("div");
        number.className = "np-number";
        number.textContent = `${index + 1} / ${count}`;
        page.appendChild(number);
      }
      pages.appendChild(page);
    }
    measure.remove();
    doc.body.appendChild(pages);
    return { count, geometry };
  }

  function noteDocumentLines() {
    return noteLines($("[data-note-markdown]")?.value || "");
  }

  function noteCurrentDocument() {
    const markdown = $("[data-note-markdown]")?.value || "";
    return {
      title: $("[data-note-title]")?.value || "", emoji: state.notesView.selected?.emoji || "",
      markdown, layout: noteLayoutRead(markdown),
    };
  }

  function notePagesMode() { return $("[data-note-work]")?.dataset.mode === "pages"; }

  function scheduleNotePages(delay = 250) {
    const view = state.notesView;
    clearTimeout(view.pagesTimer);
    view.pagesTimer = setTimeout(renderNotePagesView, delay);
  }

  function renderNotePagesView() {
    const view = state.notesView, frame = $("[data-note-pages-frame]");
    if (!frame || !notePagesMode() || !view.selected) return;
    const current = noteCurrentDocument();
    const key = JSON.stringify([view.selected.id, current.title, current.emoji, current.markdown]);
    if (key === view.pagesKey && frame.contentDocument?.querySelector(".np-pages")) { fitNotePages(); return; }
    view.pagesKey = key;
    const scroll = frame.contentDocument?.scrollingElement?.scrollTop || 0;
    frame.onload = async () => {
      const doc = frame.contentDocument;
      if (!doc) return;
      await Promise.all([...doc.images].map((image) => image.decode().catch(() => null)));
      const { count } = paginateNoteDocument(doc, current.layout, noteLines(current.markdown), { screen: true });
      view.pagesCount = count;
      const label = $("[data-note-pages-count]");
      if (label) label.textContent = `${count} ${count === 1 ? "page" : "pages"}`;
      wireNotePagesFrame(doc);
      fitNotePages();
      if (doc.scrollingElement) doc.scrollingElement.scrollTop = scroll;
      if (view.imageSelection?.where === "pages") reselectNoteImage();
    };
    frame.srcdoc = notePrintDocument(current);
  }

  function fitNotePages() {
    const view = state.notesView, frame = $("[data-note-pages-frame]"), doc = frame?.contentDocument;
    const pages = doc?.querySelector(".np-pages");
    if (!pages) return;
    const choice = $("[data-note-pages-zoom]")?.value || "fit";
    const page = pages.querySelector(".np-page");
    const natural = page ? page.offsetWidth : 800;
    const zoom = choice === "fit" ? Math.max(0.3, Math.min(2, (frame.clientWidth - 48) / natural)) : Number(choice) / 100;
    pages.style.zoom = String(zoom);
    view.pagesZoom = zoom;
    if (view.imageSelection?.where === "pages") requestAnimationFrame(positionNoteImageChrome);
  }

  function wireNotePagesFrame(doc) {
    doc.addEventListener("pointerdown", (event) => {
      const image = event.target.closest?.("img[data-note-image-line]");
      if (event.target.closest?.(".np-select span")) { startNoteImageResize(event); return; }
      if (!image) { if (!event.target.closest?.(".np-select")) selectNoteImage(null); return; }
      if (!noteCanEdit(state.notesView.selected)) return;
      event.preventDefault();
      selectNoteImage(image, "pages");
      if (["behind", "front"].includes(image.dataset.wrap)) startNoteImageMove(event, image);
    });
    doc.addEventListener("keydown", noteImageKeydown);
    doc.addEventListener("scroll", () => positionNoteImageChrome(), { passive: true });
  }

  function noteImageElements() {
    const view = state.notesView, selection = view.imageSelection;
    if (!selection) return [];
    const line = String(selection.line);
    const roots = selection.where === "pages" ? [$("[data-note-pages-frame]")?.contentDocument] : [$("[data-note-preview]")];
    return roots.filter(Boolean).flatMap((root) => [...root.querySelectorAll(`img[data-note-image-line="${line}"]`)]);
  }

  function selectNoteImage(image, where = "live") {
    const view = state.notesView;
    if (!image) {
      if (!view.imageSelection) return;
      view.imageSelection = null;
      document.querySelectorAll("[data-note-image-toolbar], [data-note-image-wrap-menu]").forEach((node) => { node.hidden = true; });
      $("[data-note-pages-frame]")?.contentDocument?.querySelector(".np-select")?.remove();
      $("[data-note-image-select]")?.setAttribute("hidden", "");
      return;
    }
    if (view.live) endNoteLiveEdit();
    view.imageSelection = { line: Number(image.dataset.noteImageLine), where, page: image.closest(".np-page")?.dataset.page || "" };
    $("[data-note-comment-float]").hidden = true;
    $("[data-note-react-float]").hidden = true;
    $("[data-note-image-toolbar]").hidden = false;
    positionNoteImageChrome();
    if (!$("[data-note-image-panel]").hidden) renderNoteImagePanel();
    refreshNoteIcons();
  }

  function reselectNoteImage() {
    const view = state.notesView;
    if (!view.imageSelection) return;
    if (!noteImageElements().length) { selectNoteImage(null); return; }
    positionNoteImageChrome();
    if (!$("[data-note-image-panel]").hidden) renderNoteImagePanel();
  }

  function noteSelectedImage() {
    const view = state.notesView, images = noteImageElements();
    return images.find((image) => image.closest(".np-page")?.dataset.page === view.imageSelection?.page) || images[0] || null;
  }

  function positionNoteImageChrome() {
    const view = state.notesView, image = noteSelectedImage(), toolbar = $("[data-note-image-toolbar]"), body = $(".notes-body");
    if (!view.imageSelection || !image || !toolbar || !body) return;
    let rect = image.getBoundingClientRect();
    if (view.imageSelection.where === "pages") {
      const frame = $("[data-note-pages-frame]"), doc = frame.contentDocument;
      let box = doc.querySelector(".np-select");
      if (!box) { box = doc.createElement("div"); box.className = "np-select"; box.appendChild(doc.createElement("span")); doc.body.appendChild(box); }
      Object.assign(box.style, { left: `${rect.left - 2}px`, top: `${rect.top - 2}px`, width: `${rect.width}px`, height: `${rect.height}px` });
      const outer = frame.getBoundingClientRect();
      rect = { left: rect.left + outer.left, right: rect.right + outer.left, top: rect.top + outer.top, bottom: rect.bottom + outer.top, width: rect.width, height: rect.height };
    } else {
      const box = $("[data-note-image-select]");
      box.hidden = false;
      const outer = body.getBoundingClientRect();
      Object.assign(box.style, { left: `${rect.left - outer.left - 2}px`, top: `${rect.top - outer.top - 2}px`, width: `${rect.width}px`, height: `${rect.height}px` });
    }
    const outer = body.getBoundingClientRect();
    toolbar.style.left = `${Math.max(8, Math.min(outer.width - toolbar.offsetWidth - 8, rect.left - outer.left + rect.width / 2 - toolbar.offsetWidth / 2))}px`;
    toolbar.style.top = `${Math.max(8, Math.min(outer.height - toolbar.offsetHeight - 8, rect.bottom - outer.top + 10))}px`;
    const menu = $("[data-note-image-wrap-menu]");
    if (menu && !menu.hidden) {
      const button = toolbar.querySelector("[data-note-image-wrap-open]").getBoundingClientRect();
      menu.style.left = `${Math.max(8, button.left - outer.left)}px`;
      menu.style.top = `${button.bottom - outer.top + 6}px`;
    }
  }

  function noteImageSettingsNow() {
    const view = state.notesView;
    if (!view.imageSelection) return null;
    return noteImageSettingsAt(noteDocumentLines(), view.imageSelection.line);
  }

  function writeNoteImageSettings(settings, { render = true } = {}) {
    const view = state.notesView, area = $("[data-note-markdown]");
    if (!view.imageSelection || !area || !noteCanEdit(view.selected)) return;
    const result = noteWithImageSettings(area.value, view.imageSelection.line, settings);
    view.imageSelection.line = result.line;
    for (const image of noteImageElements()) image.dataset.noteImageLine = String(result.line);
    if (result.markdown === area.value) return;
    area.value = result.markdown;
    for (const image of noteImageElements()) if (!["behind", "front"].includes(settings.wrap) || image.closest(".np-layer")) styleNoteImage(image, settings);
    if (render) area.dispatchEvent(new Event("input", { bubbles: true }));
    else { scheduleNoteSave(); sendNoteDraft(); noteTyped(); }
    requestAnimationFrame(positionNoteImageChrome);
  }

  function noteImageGeometry() {
    const markdown = $("[data-note-markdown]")?.value || "";
    return notePageGeometry(noteLayoutRead(markdown));
  }

  function noteImageSizeMm(image, settings) {
    const zoom = state.notesView.imageSelection?.where === "pages" ? (state.notesView.pagesZoom || 1) : 1;
    const rect = image?.getBoundingClientRect();
    const perMm = 96 / 25.4 * zoom;
    const naturalWidth = image?.naturalWidth ? image.naturalWidth * 25.4 / 96 : 0;
    const naturalHeight = image?.naturalHeight ? image.naturalHeight * 25.4 / 96 : 0;
    const width = settings.width || (image?.offsetWidth ? image.offsetWidth * 25.4 / 96 : rect ? rect.width / perMm : naturalWidth);
    const height = settings.height || (image?.offsetHeight ? image.offsetHeight * 25.4 / 96 : naturalWidth ? width * naturalHeight / naturalWidth : width);
    return { width, height, naturalWidth: naturalWidth || width, naturalHeight: naturalHeight || height };
  }

  function noteImageUnit() {
    const layout = noteLayoutRead($("[data-note-markdown]")?.value || "");
    return layout.size === "a4" ? { name: "cm", mm: 10 } : { name: "in", mm: 25.4 };
  }

  function renderNoteImagePanel() {
    const view = state.notesView, panel = $("[data-note-image-panel]"), settings = noteImageSettingsNow(), image = noteSelectedImage();
    if (!panel || !settings) return;
    const unit = noteImageUnit(), size = noteImageSizeMm(image, settings);
    const show = (value) => String(Math.round(value / unit.mm * 100) / 100);
    const set = (key, value) => { const field = panel.querySelector(`[data-note-image-field="${key}"]`); if (field && document.activeElement !== field) { if (field.type === "checkbox") field.checked = Boolean(value); else field.value = value; } };
    panel.querySelectorAll("[data-note-image-unit]").forEach((node) => { node.textContent = unit.name; });
    set("width", show(size.width));
    set("height", show(size.height));
    set("widthScale", String(Math.round(size.width / size.naturalWidth * 100)));
    set("heightScale", String(Math.round(size.height / size.naturalHeight * 100)));
    set("lock", view.imageLock !== false);
    set("rotate", String(settings.rotate));
    set("margin", show(settings.margin));
    set("x", show(settings.x));
    set("y", show(settings.y));
    set("pages", settings.pages);
    set("transparency", String(settings.transparency));
    set("brightness", String(settings.brightness));
    set("contrast", String(settings.contrast));
    const source = noteImageSource(view.imageSelection.line);
    set("alt", source?.alt || "");
    set("title", source?.title || "");
    panel.querySelectorAll("[data-note-image-wrap]").forEach((button) => button.setAttribute("aria-pressed", String(button.dataset.noteImageWrap === settings.wrap)));
    panel.querySelectorAll("[data-note-image-align]").forEach((button) => button.setAttribute("aria-pressed", String(button.dataset.noteImageAlign === settings.align)));
    panel.querySelectorAll("[data-note-image-recolor]").forEach((button) => button.setAttribute("aria-pressed", String(button.dataset.noteImageRecolor === settings.recolor)));
    const positioned = ["behind", "front"].includes(settings.wrap);
    panel.querySelector("[data-note-image-position]").disabled = !positioned;
    panel.querySelector("[data-note-image-position]").dataset.disabled = String(!positioned);
    panel.querySelector("[data-note-image-align-row]").hidden = positioned || settings.wrap === "inline";
    panel.querySelector("[data-note-image-margin-row]").hidden = settings.wrap !== "wrap";
    panel.querySelectorAll("[data-note-image-align=\"center\"]").forEach((button) => { button.hidden = settings.wrap === "wrap"; });
    const readOnly = !noteCanEdit(view.selected);
    panel.querySelectorAll("input, select, button[data-note-image-wrap], button[data-note-image-align], button[data-note-image-recolor], button[data-note-image-action]").forEach((node) => { node.disabled = readOnly || (node.closest("[data-note-image-position]") && !positioned); });
  }

  function noteImageSource(line) {
    const text = noteDocumentLines()[line] || "";
    const match = /!\[((?:\\.|[^\]\\])*)\]\(\s*(<[^>]*>|[^)\s]+)(?:\s+"((?:\\.|[^"\\])*)")?\s*\)/.exec(text);
    return match ? { match, alt: match[1].replace(/\\(.)/g, "$1"), url: match[2], title: (match[3] || "").replace(/\\(.)/g, "$1") } : null;
  }

  function writeNoteImageSource(alt, title) {
    const view = state.notesView, area = $("[data-note-markdown]");
    if (!view.imageSelection || !area || !noteCanEdit(view.selected)) return;
    const lines = noteLines(area.value), source = noteImageSource(view.imageSelection.line);
    if (!source) return;
    const escape = (text, chars) => String(text || "").replace(/[\r\n]+/g, " ").replace(chars, (char) => `\\${char}`);
    const text = `![${escape(alt, /[\\\]]/g)}](${source.url}${title ? ` "${escape(title, /[\\"]/g)}"` : ""})`;
    const line = lines[view.imageSelection.line];
    lines[view.imageSelection.line] = line.slice(0, source.match.index) + text + line.slice(source.match.index + source.match[0].length);
    area.value = lines.join("\n");
    area.dispatchEvent(new Event("input", { bubbles: true }));
  }

  function openNoteImagePanel() {
    const panel = $("[data-note-image-panel]");
    if (!panel || !state.notesView.imageSelection) return;
    panel.hidden = false;
    $("[data-note-image-wrap-menu]").hidden = true;
    renderNoteImagePanel();
    refreshNoteIcons();
  }

  function closeNoteImagePanel() {
    const panel = $("[data-note-image-panel]");
    if (panel) panel.hidden = true;
  }

  function changeNoteImage(update, options) {
    const settings = noteImageSettingsNow();
    if (!settings) return;
    const next = { ...settings, ...update };
    if (["behind", "front"].includes(next.wrap) && !["behind", "front"].includes(settings.wrap) && update.x === undefined) {
      const geometry = noteImageGeometry();
      next.x = geometry.margin; next.y = geometry.margin;
      if (!next.width) next.width = Math.round(noteImageSizeMm(noteSelectedImage(), settings).width * 10) / 10;
    }
    writeNoteImageSettings(next, options);
    renderNoteImagePanel();
  }

  function noteImageFieldChange(field) {
    const key = field.dataset.noteImageField, unit = noteImageUnit(), view = state.notesView;
    const settings = noteImageSettingsNow(), image = noteSelectedImage();
    if (!settings) return;
    const number = Number(field.value);
    if (key === "alt" || key === "title") {
      const panel = $("[data-note-image-panel]");
      writeNoteImageSource(panel.querySelector("[data-note-image-field=\"alt\"]").value, panel.querySelector("[data-note-image-field=\"title\"]").value);
      return;
    }
    if (key === "lock") { view.imageLock = field.checked; return; }
    if (key === "pages") { changeNoteImage({ pages: field.value }); return; }
    if (!Number.isFinite(number) || field.value === "") return;
    const size = noteImageSizeMm(image, settings), lock = view.imageLock !== false;
    const ratio = size.width ? size.height / size.width : 1;
    const round = (value) => Math.round(value * 10) / 10;
    if (key === "width" || key === "widthScale") {
      const width = round(key === "width" ? number * unit.mm : size.naturalWidth * number / 100);
      if (width > 0) changeNoteImage({ width, height: lock ? (settings.height ? round(width * ratio) : 0) : round(size.height) });
    } else if (key === "height" || key === "heightScale") {
      const height = round(key === "height" ? number * unit.mm : size.naturalHeight * number / 100);
      if (height > 0) changeNoteImage({ height, width: lock ? round(height / ratio) : round(size.width) });
    } else if (["margin", "x", "y"].includes(key)) changeNoteImage({ [key]: round(number * unit.mm) });
    else if (key === "rotate") changeNoteImage({ rotate: number });
    else if (["transparency", "brightness", "contrast"].includes(key)) changeNoteImage({ [key]: number }, { render: field.type !== "range" || field.dataset.committed === "true" });
  }

  function noteImageAction(action) {
    const settings = noteImageSettingsNow();
    if (!settings) return;
    const geometry = noteImageGeometry();
    if (action === "rotate90") changeNoteImage({ rotate: ((settings.rotate + 90 + 180) % 360) - 180 });
    if (action === "fill") changeNoteImage({ wrap: settings.wrap === "front" ? "front" : "behind", x: 0, y: 0, width: geometry.width, height: geometry.height, rotate: 0 });
    if (action === "center") {
      const size = noteImageSizeMm(noteSelectedImage(), settings);
      changeNoteImage({ x: Math.round((geometry.width - size.width) / 2 * 10) / 10, y: Math.round((geometry.height - size.height) / 2 * 10) / 10 });
    }
    if (action === "reset-size") changeNoteImage({ width: 0, height: 0, rotate: 0 });
    if (action === "reset-adjust") changeNoteImage({ transparency: 0, brightness: 0, contrast: 0 });
  }

  function noteImageAnchor() {
    const view = state.notesView, area = $("[data-note-markdown]");
    if (!view.imageSelection || !area) return null;
    const lines = noteLines(area.value), line = view.imageSelection.line, markdown = lines.join("\n");
    const start = noteLineOffset(lines, line) + (lines[line].length - lines[line].trimStart().length);
    const end = Math.min(start + lines[line].trim().length, start + NOTE_QUOTE_MAX);
    return {
      quote: markdown.slice(start, end), prefix: markdown.slice(Math.max(0, start - NOTE_COMMENT_CONTEXT), start),
      suffix: markdown.slice(end, end + NOTE_COMMENT_CONTEXT), start, end,
    };
  }

  function removeNoteImage() {
    const view = state.notesView, area = $("[data-note-markdown]");
    if (!view.imageSelection || !area || !noteCanEdit(view.selected)) return;
    const lines = noteLines(area.value), line = view.imageSelection.line, source = noteImageSource(line);
    if (!source) return;
    const rest = (lines[line].slice(0, source.match.index) + lines[line].slice(source.match.index + source.match[0].length)).trim();
    const from = NOTE_IMAGE_SETTINGS_LINE.test(lines[line - 1] || "") ? line - 1 : line;
    if (rest) { lines[line] = rest; if (from < line) lines.splice(from, 1); }
    else lines.splice(from, line - from + 1);
    selectNoteImage(null);
    area.value = lines.join("\n");
    area.dispatchEvent(new Event("input", { bubbles: true }));
  }

  function noteImageKeydown(event) {
    const view = state.notesView;
    if (!view.imageSelection) return;
    const typing = event.target.closest?.("input, textarea, select, [contenteditable]");
    if (event.key === "Escape") { selectNoteImage(null); closeNoteImagePanel(); return; }
    if (typing) return;
    if (event.key === "Delete" || event.key === "Backspace") { event.preventDefault(); removeNoteImage(); }
  }

  function noteImagePerMm() {
    const view = state.notesView;
    return 96 / 25.4 * (view.imageSelection?.where === "pages" ? (view.pagesZoom || 1) : 1);
  }

  function startNoteImageMove(event, image) {
    const settings = noteImageSettingsNow();
    if (!settings) return;
    const doc = image.ownerDocument, perMm = noteImagePerMm(), from = { x: event.clientX, y: event.clientY };
    let moved = null;
    const move = (next) => {
      moved = { x: Math.round((settings.x + (next.clientX - from.x) / perMm) * 10) / 10, y: Math.round((settings.y + (next.clientY - from.y) / perMm) * 10) / 10 };
      for (const copy of noteImageElements()) { copy.style.left = `${moved.x}mm`; copy.style.top = `${moved.y}mm`; }
      positionNoteImageChrome();
    };
    const up = () => {
      doc.removeEventListener("pointermove", move);
      doc.removeEventListener("pointerup", up);
      if (moved) changeNoteImage(moved);
    };
    doc.addEventListener("pointermove", move);
    doc.addEventListener("pointerup", up);
  }

  function startNoteImageResize(event) {
    const settings = noteImageSettingsNow(), image = noteSelectedImage();
    if (!settings || !image || !noteCanEdit(state.notesView.selected)) return;
    event.preventDefault();
    event.stopPropagation();
    const doc = event.target.ownerDocument, perMm = noteImagePerMm(), from = event.clientX;
    const size = noteImageSizeMm(image, settings), ratio = size.width ? size.height / size.width : 1;
    let width = 0;
    const move = (next) => {
      width = Math.max(5, Math.round((size.width + (next.clientX - from) / perMm) * 10) / 10);
      for (const copy of noteImageElements()) { copy.style.width = `${width}mm`; copy.style.height = settings.height ? `${width * ratio}mm` : "auto"; copy.style.maxWidth = "none"; }
      positionNoteImageChrome();
    };
    const up = () => {
      doc.removeEventListener("pointermove", move);
      doc.removeEventListener("pointerup", up);
      if (width) changeNoteImage({ width, height: settings.height ? Math.round(width * ratio * 10) / 10 : 0 });
    };
    doc.addEventListener("pointermove", move);
    doc.addEventListener("pointerup", up);
  }

  async function insertNoteImageFile(file, background) {
    const area = $("[data-note-markdown]");
    if (!file || !area || !noteCanEdit(state.notesView.selected)) return;
    const budget = Math.min(NOTE_IMAGE_MAX_CHARS, NOTE_MARKDOWN_MAX - area.value.length - 256);
    if (budget < 4096) { noteNotice("main", NOTE_ERRORS.note_full, "bad"); return; }
    let url = "";
    try { url = await noteImageDataUrl(file, budget); } catch (_) { noteNotice("main", NOTE_ERRORS.image_unreadable, "bad"); return; }
    if (!url) { noteNotice("main", NOTE_ERRORS.image_too_large, "bad"); return; }
    const markdown = area.value, layout = noteLayoutRead(markdown), geometry = notePageGeometry(layout);
    let base = markdown;
    if (background && noteLayoutLineIndex(noteLines(markdown)) < 0) base = noteWithLayout(markdown, layout);
    const settings = background
      ? noteImageSettingsLine({ ...NOTE_IMAGE_DEFAULTS, wrap: "behind", x: 0, y: 0, width: geometry.width, height: geometry.height, pages: "all" })
      : "";
    const trimmed = base.replace(/\s+$/, "");
    area.value = `${trimmed}${trimmed ? "\n\n" : ""}${settings ? `${settings}\n` : ""}![${background ? "background" : "image"}](${url})\n`;
    area.dispatchEvent(new Event("input", { bubbles: true }));
  }

  function wireNoteImages() {
    const preview = $("[data-note-preview]"), toolbar = $("[data-note-image-toolbar]"), panel = $("[data-note-image-panel]"), menu = $("[data-note-image-wrap-menu]");
    preview?.addEventListener("pointerdown", (event) => {
      const image = event.target.closest("img[data-note-image-line]");
      if (event.target.closest("[data-note-image-select] span")) return;
      if (!image || !noteLiveMode() || !noteCanEdit(state.notesView.selected) || image.closest("[data-editing=\"true\"]")) return;
      event.preventDefault();
      selectNoteImage(image, "live");
    });
    $("[data-note-image-select] span")?.addEventListener("pointerdown", startNoteImageResize);
    toolbar?.addEventListener("mousedown", (event) => event.preventDefault());
    toolbar?.addEventListener("click", (event) => {
      const button = event.target.closest("button"); if (!button) return;
      if (button.matches("[data-note-image-options-open]")) openNoteImagePanel();
      if (button.matches("[data-note-image-wrap-open]")) {
        menu.hidden = !menu.hidden;
        const settings = noteImageSettingsNow();
        menu.querySelectorAll("[data-note-image-wrap]").forEach((item) => item.setAttribute("aria-checked", String(item.dataset.noteImageWrap === settings?.wrap)));
        positionNoteImageChrome();
      }
      if (button.matches("[data-note-image-comment]")) { const anchor = noteImageAnchor(); if (anchor) { selectNoteImage(null); startNoteComment(anchor); } }
      if (button.matches("[data-note-image-react]")) { const anchor = noteImageAnchor(); if (anchor) openNoteReactionPicker(anchor, button); }
    });
    menu?.addEventListener("click", (event) => {
      const item = event.target.closest("[data-note-image-wrap]"); if (!item) return;
      menu.hidden = true;
      changeNoteImage({ wrap: item.dataset.noteImageWrap });
    });
    panel?.addEventListener("click", (event) => {
      const button = event.target.closest("button"); if (!button || button.disabled) return;
      if (button.matches("[data-note-image-panel-close]")) closeNoteImagePanel();
      else if (button.dataset.noteImageWrap) changeNoteImage({ wrap: button.dataset.noteImageWrap });
      else if (button.dataset.noteImageAlign) changeNoteImage({ align: button.dataset.noteImageAlign });
      else if (button.dataset.noteImageRecolor) changeNoteImage({ recolor: button.dataset.noteImageRecolor });
      else if (button.dataset.noteImageAction) noteImageAction(button.dataset.noteImageAction);
    });
    panel?.addEventListener("input", (event) => {
      const field = event.target.closest("[data-note-image-field]");
      if (field?.type === "range") { field.dataset.committed = "false"; noteImageFieldChange(field); }
    });
    panel?.addEventListener("change", (event) => {
      const field = event.target.closest("[data-note-image-field]"); if (!field) return;
      field.dataset.committed = "true";
      noteImageFieldChange(field);
    });
    document.addEventListener("keydown", noteImageKeydown);
    document.addEventListener("pointerdown", (event) => {
      if (!state.notesView.imageSelection) return;
      if (event.composedPath().some((node) => node?.matches?.("[data-note-image-toolbar], [data-note-image-wrap-menu], [data-note-image-panel], [data-note-image-select], [data-note-reaction-picker], img[data-note-image-line], [data-note-pages-frame]"))) return;
      selectNoteImage(null);
    });
    $("[data-note-pages-zoom]")?.addEventListener("change", fitNotePages);
    window.addEventListener("resize", () => { if (notePagesMode()) fitNotePages(); else positionNoteImageChrome(); });
    $("[data-note-preview-pane]")?.addEventListener("scroll", () => positionNoteImageChrome(), { passive: true });
    const picker = $("[data-note-image-file]");
    document.querySelectorAll("[data-note-insert-image]").forEach((button) => button.addEventListener("click", () => {
      picker.dataset.background = button.dataset.noteInsertImage;
      picker.value = "";
      picker.click();
    }));
    picker?.addEventListener("change", () => void insertNoteImageFile(picker.files?.[0], picker.dataset.background === "background"));
    $("[data-note-pages-export]")?.addEventListener("click", openNotePdfDialog);
  }

  function renderNoteMode(note) {
    const canEdit = noteCanEdit(note);
    const mode = canEdit ? state.notesView.mode : "preview";
    const work = $("[data-note-work]");
    if (work) work.dataset.mode = mode;
    const heading = $("[data-note-preview-title]");
    if (heading && mode === "live") {
      try { heading.contentEditable = "plaintext-only"; } catch (_) { heading.contentEditable = "true"; }
      heading.setAttribute("role", "textbox");
      heading.setAttribute("aria-label", "Note title");
    } else if (heading) {
      heading.removeAttribute("contenteditable"); heading.removeAttribute("role"); heading.removeAttribute("aria-label");
    }
    const group = $("[data-note-mode-group]");
    if (group) group.hidden = !canEdit;
    document.querySelectorAll("[data-note-mode]").forEach((button) =>
      button.setAttribute("aria-pressed", button.dataset.noteMode === mode ? "true" : "false"));
  }

  function noteShareListHtml(shares, scope) {
    const owner = state.session?.nodeName || "You";
    return `
        <div class="notes-person">
          ${noteAvatarTagHtml(owner, "notes-avatar", `style="background:${noteAvatarColor(owner)}"`, escapeHtml(noteInitials(owner)))}
          <div class="min-w-0"><div class="notes-person-name">${issuePersonNameHtml(owner, "")}</div><div class="notes-person-hint">You</div></div>
          <span class="notes-person-hint">Owner</span><span></span>
        </div>` + shares.map((share) => {
        const key = `${escapeHtml(share.type)}:${escapeHtml(share.name)}`;
        return `
        <div class="notes-person">
          ${share.type === "organization"
            ? `<span class="notes-avatar" data-type="${escapeHtml(share.type)}" style="background:${noteAvatarColor(share.name)}">${escapeHtml(noteInitials(share.name))}</span>`
            : noteAvatarTagHtml(share.name, "notes-avatar", `data-type="${escapeHtml(share.type)}" style="background:${noteAvatarColor(share.name)}"`, escapeHtml(noteInitials(share.name)))}
          <div class="min-w-0"><div class="notes-person-name">${share.type === "organization" ? escapeHtml(share.name) : issuePersonNameHtml(share.name, "")}</div><div class="notes-person-hint">${share.type === "organization" ? "Organization" : "User"}</div></div>
          <select data-${scope}-share-role-change="${key}" aria-label="Access for ${escapeHtml(share.name)}">
            <option value="editor" ${share.role === "editor" ? "selected" : ""}>Can edit</option>
            <option value="viewer" ${share.role !== "editor" ? "selected" : ""}>Can view</option>
          </select>
          <button type="button" data-${scope}-unshare="${key}" class="notes-chip-x" title="Remove access" aria-label="Remove ${escapeHtml(share.name)}">${noteIcon("x")}</button>
        </div>`;
      }).join("");
  }

  function renderNoteShares(note) {
    const shareTitle = $("[data-note-share-title]");
    if (shareTitle) shareTitle.textContent = note.title || "Untitled note";
    const people = $("[data-note-shares]");
    if (people) people.innerHTML = noteShareListHtml(note.shares || [], "note");
    const accessRow = $("[data-note-access-row]");
    if (accessRow) accessRow.hidden = !(note.shares || []).length;
    const accessLink = $("[data-note-access-link]");
    if (accessLink) {
      accessLink.href = noteAccessUrl(note);
      accessLink.textContent = noteAccessUrl(note).replace(/^https?:\/\//, "");
    }
    const isPublic = note.visibility === "public";
    const url = `${location.origin}/notes/${note.id}`;
    const toggle = $("[data-note-public]");
    if (toggle) {
      toggle.setAttribute("aria-checked", isPublic ? "true" : "false");
      toggle.disabled = note.role !== "owner";
    }
    const hint = $("[data-note-publish-hint]");
    if (hint) {
      hint.textContent = isPublic
        ? (noteViewsLabel(note) ? `Live · ${noteViewsLabel(note)}` : "Live · no readers yet")
        : "Anyone with the link can read it";
    }
    const row = $("[data-note-public-row]");
    if (row) row.hidden = !isPublic;
    const link = $("[data-note-public-link]");
    if (link) { link.href = `/notes/${note.id}`; link.textContent = url.replace(/^https?:\/\//, ""); }
    const menuLink = $("[data-note-menu-public]");
    if (menuLink) { menuLink.href = `/notes/${note.id}`; menuLink.hidden = !isPublic; }
  }

  function renderNoteLinks(note) {
    const target = $("[data-note-links]"); if (!target) return;
    const canEdit = noteCanEdit(note);
    const kindIcon = { issue: "circle-dot", pull: "git-pull-request", discussion: "message-square" };
    const chips = (note.links || []).map((link) => {
      const section = { pull: "pull", discussion: "discussions" }[link.kind] || "issues";
      const href = `${repoPathUrl({ owner: link.owner, name: link.repo })}/${section}/${encodeURIComponent(link.number)}`;
      return `<span class="notes-chip"><span data-kind="${escapeHtml(link.kind)}">${noteIcon(kindIcon[link.kind] || "link")}</span><a href="${href}" title="${escapeHtml(link.owner)}/${escapeHtml(link.repo)} ${escapeHtml(link.kind)} #${link.number}">${escapeHtml(link.repo)}#${link.number}</a>${canEdit ? `<button type="button" class="notes-chip-x" data-note-unlink='${escapeHtml(JSON.stringify(link))}' aria-label="Unlink ${escapeHtml(link.repo)}#${link.number}">${noteIcon("x")}</button>` : ""}</span>`;
    }).join("");
    const adder = !canEdit ? ""
      : state.notesView.linking
        ? `<form data-note-link-form class="notes-link-form"><input data-note-link-input autocomplete="off" placeholder="Paste a URL or owner/repo#123" aria-label="Issue or pull request" /><select data-note-link-kind aria-label="Kind"><option value="issue">Issue</option><option value="pull">PR</option><option value="discussion">Discussion</option></select></form>`
        : `<button type="button" data-note-link-start class="notes-chip notes-chip-add">${noteIcon("plus")}Link issue or PR</button>`;
    target.innerHTML = chips + adder;
    target.hidden = !chips && !adder;
    refreshNoteIcons();
  }

  function renderNoteEmojiButton(note) {
    const button = $("[data-note-emoji-open]"); if (!button) return;
    const canEdit = noteCanEdit(note), emoji = note.emoji || "";
    const label = canEdit ? (emoji ? "Change emoji" : "Add an emoji") : `Emoji ${emoji}`;
    button.innerHTML = emoji ? escapeHtml(emoji) : noteIcon("smile-plus");
    button.dataset.empty = emoji ? "false" : "true";
    button.disabled = !canEdit;
    button.hidden = !canEdit && !emoji;
    button.title = canEdit ? label : "";
    button.setAttribute("aria-label", label);
  }

  function renderNoteEditor(note, { keepDraft = false } = {}) {
    const view = state.notesView;
    $("[data-note-empty]").hidden = Boolean(note) || Boolean(view.pageId);
    $("[data-note-editor]").hidden = !note || Boolean(view.pageId);
    if (!note) { renderNoteList(); return; }
    const canEdit = noteCanEdit(note), isOwner = note.role === "owner";
    const title = $("[data-note-title]"), markdown = $("[data-note-markdown]");
    if (!keepDraft) {
      title.value = note.title || "";
      markdown.value = note.markdown || "";
      view.localDirty = false;
      view.linking = false;
      setNoteSaveState(canEdit ? "saved" : "readonly",
        canEdit && note.updatedAt ? `Saved ${relativeTimeLabel(note.updatedAt)}` : "");
    }
    title.readOnly = !canEdit;
    markdown.readOnly = !canEdit;
    $("[data-note-toolbar]").hidden = !canEdit;
    if (view.assist && (view.assist.noteId !== note.id || !canEdit)) closeNoteAssist();
    void loadNoteAssistStatus();
    const pill = $("[data-note-visibility]");
    pill.dataset.visibility = note.visibility === "public" ? "public" : "private";
    pill.innerHTML = noteIcon(noteVisibilityIcon(note)) + escapeHtml(note.visibility === "public" ? "Published" : noteStatusLabel(note));
    $("[data-note-share-open]").hidden = !isOwner;
    if (!isOwner) closeNotePopovers();
    document.querySelectorAll("[data-note-menu] [data-note-link-start]").forEach((item) => { item.hidden = !canEdit; });
    $("[data-note-delete]").hidden = !isOwner;
    $("[data-note-delete-sep]").hidden = !isOwner;
    renderNoteMode(note);
    renderNoteEmojiButton(note);
    renderNotePreview();
    renderNoteShares(note);
    renderNoteLinks(note);
    renderNoteList();
    connectNoteSocket(note);
    if (noteSideShows("history")) void loadNoteVersions();
    refreshNoteIcons();
  }

  function noteActivityCounts(counts) {
    return {
      views: Number(counts.views || 0), readers: Number(counts.readers || 0),
      actions: Number(counts.actions || 0), webViews: Number(counts.webViews || 0),
    };
  }

  function applyNoteActivity(noteId, payload) {
    const view = state.notesView, counts = payload.counts || {};
    const fields = noteActivityCounts(counts);
    view.items = view.items.map((item) => item.id === noteId ? { ...item, ...fields } : item);
    if (view.selected?.id === noteId) {
      view.selected = { ...view.selected, ...fields };
      view.activity = { noteId, counts, events: payload.events || [], error: "" };
      renderNoteActivity();
    }
    renderNoteList();
  }

  async function fetchNoteActivity(method = "GET", body = null) {
    const view = state.notesView, note = view.selected;
    if (!note || note.role === "public") return;
    clearTimeout(view.activityTimer);
    const generation = ++view.activityGeneration;
    try {
      const payload = await noteRequest(`/api/notes/${note.id}/activity`, method, body);
      if (generation === view.activityGeneration) applyNoteActivity(note.id, payload);
    } catch (error) {
      if (generation !== view.activityGeneration || view.selected?.id !== note.id) return;
      view.activity = { noteId: note.id, counts: view.activity?.counts || {}, events: view.activity?.events || [], error: error.message };
      renderNoteActivity();
    }
  }

  function recordNoteActivity(kind, detail = null) {
    return fetchNoteActivity("POST", detail ? { kind, detail } : { kind });
  }

  function scheduleNoteActivity() {
    const view = state.notesView;
    clearTimeout(view.activityTimer);
    view.activityTimer = setTimeout(() => void fetchNoteActivity(), NOTE_ACTIVITY_REFRESH_MS);
  }

  function noteActivityEntries(activity, tab) {
    const counts = activity.counts || {};
    const events = [...(activity.events || [])];
    if (Number(counts.webViews || 0) > 0) {
      events.push({ kind: "web", at: Number(counts.webLastViewedAt || 0), count: Number(counts.webViews), readers: Number(counts.webReaders || 0) });
      events.sort((a, b) => b.at - a.at);
    }
    const shown = events.filter((entry) => tab === "all"
      || (tab === "views" && ["view", "web"].includes(entry.kind))
      || (tab === "history" && NOTE_HISTORY_KINDS.includes(entry.kind))
      || (tab === "actions" && !["view", "web", "edit", "create"].includes(entry.kind)));
    const grouped = [];
    for (const entry of shown) {
      const last = grouped[grouped.length - 1];
      if (entry.kind === "edit" && last?.kind === "edit" && last.actor === entry.actor
          && last.oldestAt - entry.at <= NOTE_EDIT_GROUP_MS) {
        last.from = entry.version; last.count += 1; last.oldestAt = entry.at;
      } else {
        grouped.push(entry.kind === "edit" ? { ...entry, from: entry.version, count: 1, oldestAt: entry.at } : entry);
      }
    }
    return grouped;
  }

  function noteActivityText(entry) {
    const detail = entry.detail || {};
    const name = (value) => `<b>${escapeHtml(value || "someone")}</b>`;
    const ref = `${escapeHtml(repoPublicKey(`${detail.owner}/${detail.repo}`))}#${Number(detail.number || 0)}`;
    const role = detail.role === "editor" ? "can edit" : "can view";
    const count = Number(entry.count || 1);
    switch (entry.kind) {
      case "view": return count > 1 ? `viewed it ${count} times` : "viewed it";
      case "web": return `${noteCountLabel(count, "read", "reads")} of the published page${entry.readers > 1 ? ` by ${entry.readers} readers` : ""}`;
      case "edit": return count > 1 ? `edited it · v${entry.from}–v${entry.version}, ${count} saves` : `edited it · v${entry.version}`;
      case "create": return "created the note";
      case "restore": return `restored version ${Number(detail.version || 0)}${detail.as ? ` as v${Number(detail.as)}` : ""}`;
      case "share": return `shared it with ${name(detail.name)} · ${role}`;
      case "role": return `changed ${name(detail.name)} to ${role}`;
      case "unshare": return `stopped sharing with ${name(detail.name)}`;
      case "publish": return "published it to the web";
      case "unpublish": return "unpublished it";
      case "link": return `linked ${ref}`;
      case "unlink": return `unlinked ${ref}`;
      case "export": return "downloaded the Markdown";
      case "copy-link": return "copied the link";
      case "task": return "turned it into a task";
      case "issue": return detail.repo ? `filed issue ${ref} from it` : "filed an issue from it";
      case "comment": return detail.reply ? "replied to a comment" : "commented";
      case "resolve": return "resolved a comment";
      case "reopen": return "reopened a comment";
      default: return escapeHtml(entry.kind);
    }
  }

  function noteActivityActor(entry) {
    if (entry.kind === "web") return "";
    const me = String(state.session?.nodeName || "").toLowerCase();
    const actor = String(entry.actor || "");
    return `<b>${actor && actor === me ? "You" : actor ? issuePersonNameHtml(actor, "") : "Someone"}</b> `;
  }

  function renderNoteActivity() {
    const view = state.notesView, list = $("[data-note-activity-list]");
    if (!list) return;
    const activity = view.activity?.noteId === view.selected?.id ? view.activity : null;
    const counts = activity?.counts || {};
    const tab = view.activityTab;
    const summary = $("[data-note-activity-summary]");
    if (summary) {
      const versions = counts.versions ?? view.selected?.version;
      summary.innerHTML = [
        ["views", counts.views, notePlural(counts.views, "view", "views"), Number(counts.readers || 0) > 0 ? `${noteCountLabel(counts.views, "view", "views")} from ${noteCountLabel(counts.readers, "reader", "readers")}` : "Show views"],
        ["history", versions, notePlural(versions, "version", "versions"), "Show history"],
        ["actions", counts.actions, notePlural(counts.actions, "action", "actions"), "Show shares, links and other actions"],
      ].map(([key, value, label, hint]) => `
        <button type="button" class="notes-activity-stat" data-note-activity-tab="${key}" aria-pressed="${tab === key ? "true" : "false"}" title="${escapeHtml(hint)}">
          <span class="notes-activity-num">${activity ? Number(value || 0) : "–"}</span><span>${escapeHtml(label)}</span>
        </button>`).join("");
    }
    const all = $("[data-note-activity-all]");
    if (all) all.hidden = tab === "all";
    const history = $("[data-note-activity-history]");
    if (history) history.hidden = tab !== "history";
    if (!activity) { list.innerHTML = '<li class="notes-act-empty">Loading activity…</li>'; return; }
    const entries = noteActivityEntries(activity, tab);
    const error = activity.error ? `<li class="notes-act-empty" data-tone="bad">${escapeHtml(activity.error)}</li>` : "";
    list.innerHTML = error + (entries.map((entry) => `
      <li class="notes-act" data-kind="${escapeHtml(entry.kind)}">
        <span class="notes-act-icon">${noteIcon(NOTE_ACTIVITY_ICONS[entry.kind] || "dot")}</span>
        <div class="notes-act-body">
          <div class="notes-act-text">${noteActivityActor(entry)}${noteActivityText(entry)}</div>
          <time class="notes-act-time" title="${escapeHtml(entry.at ? new Date(entry.at).toLocaleString() : "")}">${escapeHtml(entry.at ? relativeTimeLabel(entry.at) : "")}</time>
        </div>
      </li>`).join("") || `<li class="notes-act-empty">${tab === "views" ? "Nobody has opened this note yet." : tab === "actions" ? "No shares, links or other actions yet." : "Nothing yet."}</li>`);
    refreshNoteIcons();
  }

  function noteSideShows(tab) {
    const view = state.notesView;
    return Boolean(view.commentsOpen && view.panelTab === tab);
  }

  function rememberNoteSide() {
    const view = state.notesView;
    try {
      localStorage.setItem(NOTE_ACTIVITY_KEY, view.commentsOpen ? "open" : "closed");
      localStorage.setItem(NOTE_SIDE_TAB_KEY, view.panelTab);
    } catch (_) {}
  }

  function setNoteActivityTab(tab) {
    const view = state.notesView;
    view.activityTab = ["views", "history", "actions"].includes(tab) && tab !== view.activityTab ? tab : "all";
    renderNoteActivity();
  }

  // Fold a server copy of the selected note back into the listing, so the
  // sidebar row's status and "shared with" line stay honest without
  // refetching the whole list.
  function storeNote(note) {
    const view = state.notesView;
    view.selected = note;
    view.items = view.items.map((item) => item.id === note.id ? { ...item, ...note } : item);
  }

  async function loadNotes(selectId = "") {
    const view = state.notesView;
    if (!state.session?.sessionToken) {
      view.loading = false; notesError("Sign in to use cloud notes."); renderNoteList(); return;
    }
    try {
      const payload = await noteRequest("/api/notes");
      view.items = payload.notes || []; view.loading = false; notesError(""); renderNoteList();
      if (view.pageId && !selectId) return;
      const id = selectId || (view.items.some((item) => item.id === view.selected?.id) ? view.selected.id : view.items[0]?.id);
      if (id) await selectNote(id);
      else view.selected = null;
      if (!view.selected && view.items[0] && view.items[0].id !== id) await selectNote(view.items[0].id);
      if (!view.selected) renderNoteEditor(null);
    } catch (error) {
      view.loading = false; renderNoteList();
      notesError(`Notes are unavailable: ${error.message}`);
    }
  }

  async function selectNote(id, { keepDraft = false } = {}) {
    const view = state.notesView;
    if (view.selected && view.selected.id !== id) await flushNoteSave();
    try {
      const payload = await noteRequest(`/api/notes/${id}`);
      const same = view.selected?.id === payload.note.id;
      if (!same) { view.conflict = null; view.emojiPending = null; $("[data-note-conflict]").hidden = true; selectNoteImage(null); closeNoteImagePanel(); view.pagesKey = ""; }
      storeNote(payload.note);
      rememberNoteInUrl(payload.note);
      renderNoteEditor(view.selected, { keepDraft: keepDraft && same && view.localDirty });
      if (same) scheduleNoteActivity();
      else {
        view.activity = null; renderNoteActivity(); void recordNoteActivity("view");
        view.seenAt = Date.now(); markNoteSeen(payload.note.id);
        view.comments = null; view.commentDraft = null; view.commentEditing = ""; view.commentFocus = "";
        renderNoteComments(); void loadNoteComments();
        view.chat = null; renderNoteChat(); void loadNoteChat();
      }
    } catch (error) {
      if (error.status === 404 && view.selected?.id === id) {
        noteNotice("main", NOTE_ERRORS.note_not_found, "bad");
        view.items = view.items.filter((item) => item.id !== id);
        view.selected = null; view.localDirty = false;
        await loadNotes();
      } else noteNotice("main", error.message, "bad");
    }
  }

  async function createNote() {
    noteNotice("");
    await flushNoteSave();
    closeNotePage();
    try {
      const payload = await noteRequest("/api/notes", "POST", { title: "Untitled note", markdown: "" });
      if (state.notesView.filter !== "all" && state.notesView.filter !== "mine") setNoteFilter("all");
      state.notesView.items.unshift(payload.note); state.notesView.selected = payload.note;
      state.notesView.activity = null;
      renderNoteEditor(payload.note);
      void fetchNoteActivity();
      showNoteEditorOnMobile();
      if (state.notesView.mode === "preview") setNoteMode("split");
      $("[data-note-title]")?.select();
    } catch (error) { noteNotice("main", error.message, "bad"); }
  }

  function scheduleNoteSave() {
    const view = state.notesView;
    view.localDirty = true;
    clearTimeout(view.saveTimer);
    if (view.conflict) { setNoteSaveState("dirty", "Not saved — resolve the conflict"); return; }
    setNoteSaveState("dirty");
    view.saveTimer = setTimeout(() => void saveNote(), NOTE_AUTOSAVE_MS);
  }

  async function flushNoteSave() {
    const view = state.notesView;
    clearTimeout(view.saveTimer);
    if (view.savePromise) await view.savePromise;
    if (view.localDirty && !view.conflict) await saveNote();
  }

  function saveNote(extra = {}) {
    const view = state.notesView;
    if (view.savePromise) {
      // One PATCH at a time: the next one needs the version this one returns.
      view.savePromise = view.savePromise.then(() => saveNoteNow(extra));
    } else {
      view.savePromise = saveNoteNow(extra);
    }
    const pending = view.savePromise;
    return pending.finally(() => { if (view.savePromise === pending) view.savePromise = null; });
  }

  async function saveNoteNow(extra) {
    const view = state.notesView, note = view.selected;
    clearTimeout(view.saveTimer);
    if (!note || !noteCanEdit(note) || view.conflict) return;
    const emoji = view.emojiPending;
    if (!view.localDirty && !("visibility" in extra) && emoji === null) return;
    const title = $("[data-note-title]").value, markdown = $("[data-note-markdown]").value;
    setNoteSaveState("saving");
    try {
      const payload = await noteRequest(`/api/notes/${note.id}`, "PATCH", {
        baseVersion: note.version, title, markdown, ...(emoji === null ? {} : { emoji }), ...extra,
      });
      if (view.emojiPending === emoji) view.emojiPending = null;
      if (view.selected?.id !== note.id) {
        view.items = view.items.map((item) => item.id === note.id ? { ...item, ...payload.note } : item);
        renderNoteList();
        return;
      }
      // Typing continued while the request was in flight: keep those
      // keystrokes on screen and save them against the new version.
      const changedSince = $("[data-note-title]").value !== title || $("[data-note-markdown]").value !== markdown;
      storeNote(payload.note);
      view.localDirty = changedSince;
      renderNoteEditor(view.selected, { keepDraft: true });
      scheduleNoteActivity();
      if (changedSince) scheduleNoteSave(); else setNoteSaveState("saved");
    } catch (error) {
      if (error.status === 409 && error.payload?.note) {
        view.conflict = error.payload.note;
        $("[data-note-conflict]").hidden = false;
        setNoteSaveState("dirty", "Not saved — resolve the conflict");
      } else {
        setNoteSaveState("error", error.message);
      }
    }
  }

  function noteVersionHint(version) {
    return `${escapeHtml(relativeTimeLabel(version.createdAt))}${version.author ? ` · by ${issuePersonNameHtml(`@${version.author}`, "")}` : ""}`;
  }

  function scheduleNoteVersions() {
    const view = state.notesView;
    clearTimeout(view.versionsTimer);
    if (noteSideShows("history")) view.versionsTimer = setTimeout(() => void loadNoteVersions(), NOTE_ACTIVITY_REFRESH_MS);
  }

  async function loadNoteVersions() {
    const view = state.notesView, note = view.selected; if (!note) return;
    const target = $("[data-note-versions]");
    const generation = ++view.versionsGeneration;
    try {
      const payload = await noteRequest(`/api/notes/${note.id}/versions`);
      if (generation !== view.versionsGeneration || view.selected?.id !== note.id) return;
      const canEdit = noteCanEdit(view.selected), canView = view.selected.role !== "public";
      target.innerHTML = (payload.versions || []).map((version) => {
        const current = version.version === view.selected.version;
        return `
        <div class="notes-version" data-current="${current ? "true" : "false"}">
          <span class="notes-version-node"></span>
          <div><div class="notes-version-name">Version ${version.version}</div><div class="notes-version-hint">${noteVersionHint(version)}</div></div>
          <span class="notes-version-actions">${canView ? `<button type="button" data-note-version-show="${version.version}">View</button>` : ""}${current ? '<span class="notes-version-tag">Current</span>'
            : canEdit ? `<button type="button" data-note-restore="${version.version}">Restore</button>` : ""}</span>
        </div>`;
      }).join("") || '<p class="p-3 text-xs text-muted-foreground">No saved versions yet.</p>';
      if (view.versionShown && (view.versionShown.noteId !== note.id || !(payload.versions || []).some((version) => version.version === view.versionShown.version))) closeNoteVersion();
      else if (view.versionShown) renderNoteVersion();
    } catch (error) {
      if (generation !== view.versionsGeneration) return;
      target.innerHTML = `<div class="fm-flash fm-flash-danger"><i data-lucide="circle-alert"></i><div class="fm-flash-body">${escapeHtml(error.message)}</div></div>`;
      refreshNoteIcons();
    }
  }

  function renderNoteVersion() {
    const view = state.notesView, shown = view.versionShown, box = $("[data-note-version-view]");
    if (!box) return;
    box.hidden = !shown;
    $("[data-note-versions]").hidden = Boolean(shown);
    if (!shown) { box.innerHTML = ""; return; }
    const current = shown.version === view.selected?.version;
    box.innerHTML = `
      <div class="notes-version-view-head">
        <button type="button" class="notes-icon-btn" data-note-version-back aria-label="Back to all versions" title="Back to all versions"><i data-lucide="chevron-left"></i></button>
        <div><div class="notes-version-name">Version ${shown.version}${current ? " · Current" : ""}</div><div class="notes-version-hint">${noteVersionHint(shown)}</div></div>
        ${!current && noteCanEdit(view.selected) ? `<button type="button" data-note-restore="${shown.version}">Restore</button>` : ""}
      </div>
      <div class="notes-version-view-body">
        <h1>${shown.emoji ? `${escapeHtml(shown.emoji)} ` : ""}${escapeHtml(shown.title || "Untitled note")}</h1>
        <article class="notes-preview">${renderMarkdown(shown.markdown || "", { noteDocument: true })}</article>
      </div>`;
    refreshNoteIcons();
  }

  async function showNoteVersion(number) {
    const view = state.notesView, note = view.selected; if (!note) return;
    try {
      const payload = await noteRequest(`/api/notes/${note.id}/versions?version=${encodeURIComponent(number)}`);
      if (view.selected?.id !== note.id) return;
      view.versionShown = payload.version ? { ...payload.version, noteId: note.id } : null;
      renderNoteVersion();
      $("[data-note-version-back]")?.focus();
    } catch (error) { noteNotice("versions", error.message, "bad"); }
  }

  function closeNoteVersion() {
    state.notesView.versionShown = null;
    renderNoteVersion();
  }

  function noteMe() { return String(state.session?.nodeName || "").toLowerCase(); }

  function notePresenceState() {
    const view = state.notesView;
    return noteCanEdit(view.selected) && Date.now() - (view.lastTypedAt || 0) < NOTE_EDITING_MS ? "editing" : "viewing";
  }

  function sendNotePresence() {
    const socket = state.notesView.socket;
    if (socket?.readyState !== WebSocket.OPEN) return;
    socket.send(JSON.stringify({ type: "presence", clientId: state.notesView.clientId, state: notePresenceState() }));
  }

  function noteTyped() {
    const view = state.notesView;
    if (view.follow) setNoteFollow("");
    sendNoteCursor();
    const was = notePresenceState();
    view.lastTypedAt = Date.now();
    if (was !== "editing") { sendNotePresence(); renderNotePresence(); }
    clearTimeout(view.editingTimer);
    view.editingTimer = setTimeout(() => { sendNotePresence(); renderNotePresence(); }, NOTE_EDITING_MS + 50);
  }

  function notePeople(now = Date.now()) {
    const view = state.notesView, byName = new Map();
    for (const [id, peer] of view.peers) {
      if (now - peer.seen > NOTE_PRESENCE_MS * 2.5) { view.peers.delete(id); continue; }
      const editing = peer.state === "editing" || now - (peer.draftAt || 0) < 5000;
      const known = byName.get(peer.name);
      if (!known || (editing && known.state !== "editing")) byName.set(peer.name, { name: peer.name, state: editing ? "editing" : "viewing" });
    }
    return [...byName.values()].sort((a, b) => (a.state === "editing" ? 0 : 1) - (b.state === "editing" ? 0 : 1) || a.name.localeCompare(b.name));
  }

  function notePresenceLabel(people, open) {
    if (!open) return "Offline — edits still save";
    const editing = people.filter((person) => person.state === "editing");
    if (editing.length === 1) return `${editing[0].name} is editing…`;
    if (editing.length > 1) return `${editing[0].name} and ${noteCountLabel(editing.length - 1, "other", "others")} are editing…`;
    if (people.length === 1) return `Live · ${people[0].name} is viewing`;
    return people.length ? `Live · ${people.length} others here` : "Live";
  }

  function renderNotePresence() {
    const view = state.notesView;
    const people = notePeople();
    if (view.follow && !people.some((person) => person.name === view.follow)) {
      if (view.socket?.readyState === WebSocket.OPEN) noteNotice("main", `${view.follow} left the note.`);
      setNoteFollow("");
    }
    const faces = $("[data-note-presence]");
    if (faces) {
      faces.hidden = !people.length;
      faces.innerHTML = people.slice(0, 4).map((person) =>
        `<span class="notes-face" data-state="${person.state}" style="background:${noteAvatarColor(person.name)}" title="${escapeHtml(person.name)} · ${person.state}">${escapeHtml(noteInitials(person.name))}${person.state === "editing" ? `<span class="notes-face-badge">${noteIcon("pencil")}</span>` : ""}</span>`).join("")
        + (people.length > 4 ? `<span class="notes-face notes-face-more" title="${escapeHtml(people.slice(4).map((person) => person.name).join(", "))}">+${people.length - 4}</span>` : "");
      faces.setAttribute("aria-label", people.length ? `People here: ${people.map((person) => `${person.name} ${person.state}`).join(", ")}` : "People here");
    }
    const live = $("[data-note-live]");
    if (live) live.textContent = notePresenceLabel(people, view.socket?.readyState === WebSocket.OPEN);
    const list = $("[data-note-people-list]");
    if (list && !$("[data-note-people-panel]").hidden) {
      const me = noteMe();
      const rows = [...(me ? [{ name: me, state: notePresenceState(), self: true }] : []), ...people.filter((person) => person.name !== me)];
      list.innerHTML = rows.map((person) => `<div class="notes-here">
          <span class="notes-avatar" style="background:${noteAvatarColor(person.name)}">${escapeHtml(noteInitials(person.name))}</span>
          <span class="notes-person-name">${person.self ? "You" : escapeHtml(person.name)}</span>
          <span class="notes-here-state" data-state="${person.state}">${noteIcon(person.state === "editing" ? "pencil" : "eye")}${person.state === "editing" ? "Editing" : "Viewing"}</span>
          ${person.self ? "<span></span>" : `<button type="button" class="notes-here-follow" data-note-follow-person="${escapeHtml(person.name)}" aria-pressed="${view.follow === person.name ? "true" : "false"}" title="Keep ${escapeHtml(person.name)}'s place in view">${view.follow === person.name ? "Following" : "Follow"}</button>`}
        </div>`).join("") + (people.length ? "" : `<p class="notes-comments-empty">Nobody else has this note open.</p>`);
      refreshNoteIcons();
    }
    if (faces?.innerHTML.includes("data-lucide")) refreshNoteIcons();
    renderNotePeerMarks();
  }

  function connectNoteSocket(note) {
    const view = state.notesView;
    if (view.socket && view.socketNoteId === note.id && view.socket.readyState <= WebSocket.OPEN) return;
    view.socket?.close(); view.socket = null;
    view.peers = new Map();
    if (view.follow) setNoteFollow("");
    clearInterval(view.presenceTimer);
    const token = state.session?.sessionToken;
    if (!token || !note.webSocketUrl) { renderNotePresence(); return; }
    const url = new URL(note.webSocketUrl, location.href); url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
    url.searchParams.set("sessionToken", token);
    const socket = new WebSocket(url);
    view.socket = socket; view.socketNoteId = note.id;
    view.clientId = view.clientId || crypto.randomUUID();
    socket.onopen = () => {
      sendNotePresence(); renderNotePresence();
      view.presenceTimer = setInterval(() => { sendNotePresence(); renderNotePresence(); }, NOTE_PRESENCE_MS);
    };
    socket.onclose = () => {
      if (view.socket !== socket) return;
      clearInterval(view.presenceTimer); view.socket = null; view.peers.clear(); renderNotePresence();
    };
    socket.onmessage = (event) => {
      let frame; try { frame = JSON.parse(event.data); } catch (_) { return; }
      if (view.selected?.id !== note.id) return;
      if (["presence", "cursor", "draft"].includes(frame.type) && frame.clientId && frame.clientId !== view.clientId) {
        const known = view.peers.has(frame.clientId);
        const peer = view.peers.get(frame.clientId) || { draftAt: 0 };
        const peerState = frame.type === "draft" || frame.state === "editing" ? "editing"
          : frame.type === "cursor" ? peer.state || "viewing" : "viewing";
        const moved = Number(frame.line) > 0 ? { line: Number(frame.line), column: Number(frame.column) || 0, movedAt: Date.now() } : {};
        view.peers.set(frame.clientId, { ...peer, ...moved, name: String(frame.name || "Collaborator").slice(0, 32), seen: Date.now(), state: peerState });
        if (!known) sendNotePresence();
      }
      if (frame.type === "leave" && frame.clientId) view.peers.delete(frame.clientId);
      if (frame.type === "note-comments") scheduleNoteComments();
      if (frame.type === "note-chat") scheduleNoteChat();
      if (["note-comments", "note-updated"].includes(frame.type)) noteStillWatching();
      if (frame.type === "draft" && frame.clientId && frame.clientId !== view.clientId) {
        view.peers.get(frame.clientId).draftAt = Date.now();
        // Never overwrite keystrokes that haven't been saved yet.
        if (frame.baseVersion === view.selected?.version && !view.localDirty && !view.savePromise) {
          $("[data-note-title]").value = String(frame.title || "Untitled note");
          $("[data-note-markdown]").value = String(frame.markdown || "");
          renderNotePreview();
        }
      }
      if (["note-activity", "note-updated"].includes(frame.type)) scheduleNoteActivity();
      if (frame.type === "note-updated") scheduleNoteVersions();
      if (frame.type === "note-updated" && frame.version > (view.selected?.version || 0) && !view.savePromise) {
        // With unsaved local edits the next autosave meets the 409 and asks.
        if (!view.localDirty) void selectNote(note.id);
      }
      if (view.follow && frame.clientId && Number(frame.line) > 0 && view.peers.get(frame.clientId)?.name === view.follow) {
        noteFollowTo(Number(frame.line), Number(frame.column) || 0);
      }
      if (frame.type === "note-deleted") {
        noteNotice("main", "This note was deleted.", "bad");
        view.selected = null; view.localDirty = false; void loadNotes();
      }
      if (frame.type === "access-changed") {
        socket.close();
        setTimeout(() => { if (view.selected?.id === note.id) void selectNote(note.id, { keepDraft: true }); }, 400);
      }
      renderNotePresence();
    };
  }

  function sendNoteDraft() {
    clearTimeout(state.notesView.draftTimer);
    state.notesView.draftTimer = setTimeout(() => {
      const socket = state.notesView.socket, note = state.notesView.selected;
      if (!note || socket?.readyState !== WebSocket.OPEN) return;
      socket.send(JSON.stringify({
        type: "draft", clientId: state.notesView.clientId,
        baseVersion: note.version, title: $("[data-note-title]").value,
        markdown: $("[data-note-markdown]").value, ...noteCursorFields(),
      }));
    }, 180);
  }

  function noteLiveMode() { return $("[data-note-work]")?.dataset.mode === "live"; }

  function noteLineOffset(lines, line) {
    let offset = 0;
    for (let index = 0; index < line && index < lines.length; index += 1) offset += lines[index].length + 1;
    return offset;
  }

  function noteLiveBlocks(lines, offset = 0) {
    const ranges = [];
    let start = -1, fence = "";
    for (let index = 0; index < lines.length; index += 1) {
      const line = lines[index];
      if (fence) { if (line.trim().startsWith(fence)) fence = ""; continue; }
      const open = line.match(/^\s{0,3}(`{3,}|~{3,})/);
      if (open) { if (start === -1) start = index; fence = open[1]; continue; }
      if (!line.trim()) { if (start !== -1) ranges.push([start, index]); start = -1; continue; }
      if (start === -1) start = index;
    }
    if (start !== -1) ranges.push([start, lines.length]);
    return ranges.map(([from, to]) => ({ line: from + offset, source: lines.slice(from, to).join("\n") }));
  }

  function noteLiveLocate(lines, editor, near) {
    const matches = (at) => editor.every((line, index) => lines[at + index] === line);
    if (near + editor.length <= lines.length && matches(near)) return near;
    let best = -1;
    for (let at = 0; at + editor.length <= lines.length; at += 1) {
      if (matches(at) && (best === -1 || Math.abs(at - near) < Math.abs(best - near))) best = at;
    }
    return best;
  }

  function noteLiveBlockElement(block, reuse) {
    let element = reuse.get(block.source)?.shift();
    if (!element) {
      element = document.createElement("div");
      element.className = "notes-live-block";
      element.innerHTML = renderMarkdown(block.source, { noteDocument: true });
      element.noteSource = block.source;
    }
    element.dataset.liveLine = String(block.line);
    return element;
  }

  function renderNoteLive(preview, markdown) {
    const view = state.notesView;
    const lines = markdown.split("\n");
    let editing = view.live && view.live.noteId === view.selected?.id && preview.contains(view.live.wrapper) ? view.live : null;
    if (!editing) view.live = null;
    let head = [], tail = [];
    if (editing) {
      const editorLines = editing.area.value.split("\n");
      const at = noteLiveLocate(lines, editorLines, editing.start);
      if (at === -1) { view.live = null; editing = null; }
      else {
        editing.start = at; editing.count = editorLines.length;
        editing.wrapper.dataset.liveLine = String(at);
        head = noteLiveBlocks(lines.slice(0, at));
        tail = noteLiveBlocks(lines.slice(at + editorLines.length), at + editorLines.length);
      }
    }
    if (!editing) head = noteLiveBlocks(lines);
    const reuse = new Map();
    for (const child of preview.children) {
      if (typeof child.noteSource !== "string") continue;
      if (!reuse.has(child.noteSource)) reuse.set(child.noteSource, []);
      reuse.get(child.noteSource).push(child);
    }
    const before = head.map((block) => noteLiveBlockElement(block, reuse));
    const after = tail.map((block) => noteLiveBlockElement(block, reuse));
    const keep = new Set([...before, ...after, editing?.wrapper].filter(Boolean));
    for (const node of [...preview.childNodes]) if (!keep.has(node)) node.remove();
    if (editing) {
      for (const element of before) preview.insertBefore(element, editing.wrapper);
      for (const element of after) preview.appendChild(element);
    } else preview.append(...before);
    if (!keep.size) {
      const empty = document.createElement("p");
      empty.className = "notes-live-empty";
      empty.textContent = "Click here to start writing.";
      preview.appendChild(empty);
    }
  }

  function unmarkNoteComments(root) {
    root.querySelectorAll(".notes-comment-mark").forEach((mark) => {
      const parent = mark.parentNode;
      mark.replaceWith(...mark.childNodes);
      parent?.normalize();
    });
  }

  function growNoteLiveInput(area) {
    area.style.height = "auto";
    area.style.height = `${area.scrollHeight}px`;
  }

  function startNoteLiveEdit(wrapper, caret = -1) {
    const view = state.notesView;
    if (!wrapper || !noteCanEdit(view.selected) || view.live?.wrapper === wrapper) return;
    if (view.live) endNoteLiveEdit();
    if (!wrapper.isConnected) return;
    const source = typeof wrapper.noteSource === "string" ? wrapper.noteSource : "";
    const area = document.createElement("textarea");
    area.className = "notes-live-input";
    area.dataset.noteLiveInput = "";
    area.rows = 1;
    area.spellcheck = true;
    area.setAttribute("aria-label", "Edit this part of the note");
    area.value = source;
    wrapper.replaceChildren(area);
    wrapper.dataset.editing = "true";
    wrapper.noteSource = null;
    view.live = { noteId: view.selected.id, wrapper, area, start: Number(wrapper.dataset.liveLine) || 0, count: source.split("\n").length };
    growNoteLiveInput(area);
    area.focus({ preventScroll: true });
    const at = caret < 0 ? area.value.length : Math.min(caret, area.value.length);
    area.setSelectionRange(at, at);
    view.selectionSource = "live";
    sendNoteCursor();
  }

  function startNoteLiveBlockAtEnd() {
    const view = state.notesView, area = $("[data-note-markdown]"), preview = $("[data-note-preview]");
    if (!area || !preview || !noteCanEdit(view.selected)) return;
    if (view.live) endNoteLiveEdit();
    const trimmed = area.value.replace(/\s+$/, "");
    area.value = trimmed ? `${trimmed}\n\n` : "";
    const wrapper = document.createElement("div");
    wrapper.className = "notes-live-block";
    wrapper.dataset.liveLine = String(area.value.split("\n").length - 1);
    wrapper.noteSource = "";
    preview.querySelector(".notes-live-empty")?.remove();
    preview.appendChild(wrapper);
    startNoteLiveEdit(wrapper, 0);
  }

  function endNoteLiveEdit(render = true) {
    const view = state.notesView, live = view.live;
    if (!live) return;
    view.live = null;
    delete live.wrapper.dataset.editing;
    const area = $("[data-note-markdown]");
    if (area && !live.area.value.trim()) {
      const lines = area.value.split("\n");
      lines.splice(live.start, live.count);
      let next = lines.join("\n");
      const saved = String(view.selected?.markdown || "");
      if (next.trimEnd() === saved.trimEnd()) next = saved;
      if (next !== area.value) {
        area.value = next;
        if (render) { onNoteEdited(); return; }
      }
    }
    if (render) renderNotePreview();
  }

  function noteLiveInput() {
    const live = state.notesView.live, area = $("[data-note-markdown]");
    if (!live || !area) return;
    const lines = area.value.split("\n"), next = live.area.value.split("\n");
    lines.splice(live.start, live.count, ...next);
    live.count = next.length;
    area.value = lines.join("\n");
    growNoteLiveInput(live.area);
    onNoteEdited();
  }

  function noteLiveCaret(block, event) {
    const source = typeof block.noteSource === "string" ? block.noteSource : "";
    let node = null, offset = 0;
    if (document.caretPositionFromPoint) {
      const position = document.caretPositionFromPoint(event.clientX, event.clientY);
      node = position?.offsetNode; offset = position?.offset || 0;
    } else if (document.caretRangeFromPoint) {
      const range = document.caretRangeFromPoint(event.clientX, event.clientY);
      node = range?.startContainer; offset = range?.startOffset || 0;
    }
    if (!node || node.nodeType !== Node.TEXT_NODE || !block.contains(node)) return -1;
    const text = node.nodeValue || "";
    const at = text.trim() ? source.indexOf(text) : -1;
    if (at !== -1) return at + offset;
    const before = text.slice(Math.max(0, offset - 12), offset);
    const near = before ? source.indexOf(before) : -1;
    return near === -1 ? -1 : near + before.length;
  }

  function noteLiveClick(event) {
    const view = state.notesView, preview = $("[data-note-preview]");
    if (!noteLiveMode() || !noteCanEdit(view.selected)) return;
    if (event.target.closest("a, button, input, textarea, [data-note-thread-mark], .notes-reactions, img[data-note-image-line]")) return;
    if (String(window.getSelection()?.toString() || "")) return;
    const block = event.target.closest(".notes-live-block");
    if (block && preview.contains(block)) { startNoteLiveEdit(block, noteLiveCaret(block, event)); return; }
    const last = [...preview.children].reverse().find((child) => child.matches(".notes-live-block"));
    if (last && event.clientY < last.getBoundingClientRect().bottom) return;
    startNoteLiveBlockAtEnd();
  }

  function noteLiveKeydown(event) {
    const live = state.notesView.live;
    if (!live || event.target !== live.area) return;
    const area = live.area, mod = event.metaKey || event.ctrlKey;
    if (event.key === "Escape") { event.preventDefault(); event.stopPropagation(); endNoteLiveEdit(); return; }
    if (mod && ["b", "i"].includes(event.key.toLowerCase())) {
      event.preventDefault(); event.stopPropagation();
      formatNoteSelection(event.key.toLowerCase() === "b" ? "bold" : "italic");
      return;
    }
    const sibling = (forward) => {
      let node = forward ? live.wrapper.nextElementSibling : live.wrapper.previousElementSibling;
      while (node && !node.matches(".notes-live-block")) node = forward ? node.nextElementSibling : node.previousElementSibling;
      return node;
    };
    if (event.key === "ArrowUp" && area.selectionStart === 0 && area.selectionEnd === 0) {
      const previous = sibling(false);
      if (previous) { event.preventDefault(); startNoteLiveEdit(previous); }
      return;
    }
    if (event.key === "ArrowDown" && area.selectionStart === area.value.length && area.selectionEnd === area.value.length) {
      const next = sibling(true);
      if (next) { event.preventDefault(); startNoteLiveEdit(next, 0); }
    }
  }

  function noteCaretOffset() {
    const view = state.notesView, area = $("[data-note-markdown]");
    if (!area) return -1;
    if (view.live && document.activeElement === view.live.area) {
      return noteLineOffset(area.value.split("\n"), view.live.start) + view.live.area.selectionStart;
    }
    return document.activeElement === area ? area.selectionStart : -1;
  }

  function noteCursorFields() {
    const offset = noteCaretOffset();
    if (offset < 0) return {};
    const before = $("[data-note-markdown]").value.slice(0, offset);
    return { line: before.split("\n").length, column: offset - before.lastIndexOf("\n") - 1 };
  }

  function sendNoteCursor() {
    const view = state.notesView;
    clearTimeout(view.cursorTimer);
    view.cursorTimer = setTimeout(() => {
      const socket = view.socket, cursor = noteCursorFields();
      if (socket?.readyState !== WebSocket.OPEN || !cursor.line) return;
      const key = `${view.selected?.id}:${cursor.line}:${cursor.column}`;
      if (view.sentCursor === key) return;
      view.sentCursor = key;
      socket.send(JSON.stringify({ type: "cursor", clientId: view.clientId, ...cursor }));
    }, 120);
  }

  function noteTextareaTop(area, offset) { return noteTextareaTops(area, [offset])[0]; }

  function noteTextareaTops(area, offsets) {
    const style = getComputedStyle(area), mirror = document.createElement("div");
    for (const name of ["boxSizing", "width", "paddingTop", "paddingRight", "paddingBottom", "paddingLeft",
      "borderTopWidth", "borderRightWidth", "borderBottomWidth", "borderLeftWidth", "fontFamily", "fontSize",
      "fontWeight", "fontStyle", "letterSpacing", "lineHeight", "tabSize", "textIndent", "wordSpacing", "wordBreak"]) mirror.style[name] = style[name];
    Object.assign(mirror.style, { position: "absolute", visibility: "hidden", top: "0", left: "-9999px", whiteSpace: "pre-wrap", overflowWrap: "break-word", borderStyle: "solid" });
    const markers = [];
    let at = 0;
    for (const offset of offsets) {
      mirror.appendChild(document.createTextNode(area.value.slice(at, Math.max(at, offset))));
      at = Math.max(at, offset);
      const marker = document.createElement("span");
      marker.textContent = "​";
      markers.push(mirror.appendChild(marker));
    }
    document.body.appendChild(mirror);
    const tops = markers.map((marker) => marker.offsetTop);
    mirror.remove();
    return tops;
  }

  function noteOutlineHeadings(markdown) {
    const headings = [];
    const lines = String(markdown || "").split("\n");
    let fence = "", offset = 0;
    lines.forEach((line, index) => {
      const opener = line.match(/^\s*(```|~~~)/);
      if (fence) { if (opener && opener[1] === fence) fence = ""; }
      else if (opener) fence = opener[1];
      else {
        const match = line.match(/^(#{1,6})\s+(.*)$/);
        if (match) {
          const text = match[2].replace(/\s+#+\s*$/, "").replace(/!?\[([^\]]*)\]\([^)]*\)/g, "$1").replace(/[*_`~]/g, "").trim();
          headings.push({ line: index, offset, level: match[1].length, text });
        }
      }
      offset += line.length + 1;
    });
    return headings;
  }

  function noteOutlineKey(text) { return String(text || "").toLowerCase().replace(/[^\p{L}\p{N}]+/gu, ""); }

  function noteOutlineTargets(preview, headings) {
    const found = [...preview.querySelectorAll("h1, h2, h3, h4, h5, h6")];
    let next = 0;
    return headings.map((heading) => {
      const key = noteOutlineKey(heading.text);
      for (let index = next; index < found.length && index < next + 4; index += 1) {
        if (noteOutlineKey(found[index].textContent) === key) { next = index + 1; return found[index]; }
      }
      return null;
    });
  }

  function noteScrollGeometry() {
    const view = state.notesView;
    if (view.scrollGeometry) return view.scrollGeometry;
    const area = $("[data-note-markdown]"), pane = $("[data-note-preview-pane]"), preview = $("[data-note-preview]");
    if (!area || !pane || !preview) return null;
    const headings = noteOutlineHeadings(area.value);
    const sourceShown = Boolean(area.offsetParent), previewShown = Boolean(pane.offsetParent);
    const sourceTops = sourceShown ? noteTextareaTops(area, headings.map((heading) => heading.offset)) : headings.map(() => null);
    const paneTop = pane.getBoundingClientRect().top - pane.scrollTop;
    const previewTops = previewShown
      ? noteOutlineTargets(preview, headings).map((target) => target ? target.getBoundingClientRect().top - paneTop : null)
      : headings.map(() => null);
    const sourceMax = Math.max(0, area.scrollHeight - area.clientHeight), previewMax = Math.max(0, pane.scrollHeight - pane.clientHeight);
    const anchors = [[0, 0]];
    headings.forEach((_, index) => {
      const source = sourceTops[index], target = previewTops[index], last = anchors[anchors.length - 1];
      if (source == null || target == null) return;
      if (source > last[0] && target > last[1] && source < sourceMax && target < previewMax) anchors.push([source, target]);
    });
    anchors.push([sourceMax, previewMax]);
    view.scrollGeometry = { headings, sourceTops, previewTops, anchors, sourceMax, previewMax };
    return view.scrollGeometry;
  }

  function forgetNoteScrollGeometry() { state.notesView.scrollGeometry = null; }

  function noteScrollMap(anchors, value, from, to) {
    for (let index = 1; index < anchors.length; index += 1) {
      const start = anchors[index - 1], end = anchors[index];
      if (value > end[from] && index < anchors.length - 1) continue;
      const span = end[from] - start[from];
      const ratio = span > 0 ? Math.max(0, Math.min(1, (value - start[from]) / span)) : 0;
      return start[to] + ratio * (end[to] - start[to]);
    }
    return 0;
  }

  function syncNoteScroll(from) {
    const view = state.notesView;
    if ($("[data-note-work]")?.dataset.mode !== "split" || view.scrollLead !== from || view.follow) return;
    const area = $("[data-note-markdown]"), pane = $("[data-note-preview-pane]");
    const source = from === "source" ? area : pane, target = from === "source" ? pane : area;
    if (!source || !target) return;
    const geometry = noteScrollGeometry();
    if (!geometry) return;
    const [fromIndex, toIndex, max] = from === "source" ? [0, 1, geometry.previewMax] : [1, 0, geometry.sourceMax];
    const top = Math.round(Math.max(0, Math.min(max, noteScrollMap(geometry.anchors, source.scrollTop, fromIndex, toIndex))));
    if (Math.abs(target.scrollTop - top) >= 1) target.scrollTop = top;
  }

  function renderNoteOutline(markdown) {
    const view = state.notesView, nav = $("[data-note-outline]");
    if (!nav) return;
    const headings = noteOutlineHeadings(markdown);
    const key = JSON.stringify(headings.map((heading) => [heading.level, heading.text]));
    if (key === view.outlineKey) return;
    view.outlineKey = key;
    view.outlineCurrent = -1;
    const top = Math.min(6, ...headings.map((heading) => heading.level));
    nav.innerHTML = headings.length
      ? headings.map((heading, index) => `<button type="button" class="notes-outline-entry" data-note-outline-entry="${index}" style="--depth:${Math.min(3, heading.level - top)}" title="${escapeHtml(heading.text)}">${escapeHtml(heading.text || "Untitled section")}</button>`).join("")
      : '<p class="notes-outline-empty">Headings you add (# Title) show up here.</p>';
    renderNoteOutlineCurrent();
  }

  function renderNoteOutlineOpen() {
    const open = state.notesView.outlineOpen;
    const body = $(".notes-body");
    if (body) body.dataset.outline = open ? "open" : "closed";
    const toggle = $("[data-note-outline-toggle]");
    if (toggle) {
      toggle.setAttribute("aria-pressed", open ? "true" : "false");
      toggle.setAttribute("aria-label", open ? "Hide outline" : "Show outline");
    }
  }

  function setNoteOutlineOpen(open) {
    state.notesView.outlineOpen = open;
    try { localStorage.setItem(NOTE_OUTLINE_KEY, open ? "open" : "closed"); } catch (_) {}
    renderNoteOutlineOpen();
    forgetNoteScrollGeometry();
    if (open) renderNoteOutlineCurrent();
  }

  function renderNoteOutlineCurrent() {
    const view = state.notesView, nav = $("[data-note-outline]");
    if (!nav || !view.outlineOpen || nav.offsetParent === null) return;
    const geometry = noteScrollGeometry();
    if (!geometry) return;
    const write = $("[data-note-work]")?.dataset.mode === "write";
    const tops = write ? geometry.sourceTops : geometry.previewTops;
    const scroll = (write ? $("[data-note-markdown]") : $("[data-note-preview-pane]"))?.scrollTop || 0;
    let current = -1;
    tops.forEach((top, index) => { if (top != null && top <= scroll + 24) current = index; });
    if (current === view.outlineCurrent) return;
    view.outlineCurrent = current;
    nav.querySelectorAll("[data-note-outline-entry]").forEach((entry) =>
      entry.setAttribute("aria-current", Number(entry.dataset.noteOutlineEntry) === current ? "true" : "false"));
  }

  function jumpToNoteHeading(index) {
    const view = state.notesView, geometry = noteScrollGeometry(), heading = geometry?.headings[index];
    if (!heading) return;
    const mode = $("[data-note-work]")?.dataset.mode || "split";
    const area = $("[data-note-markdown]"), pane = $("[data-note-preview-pane]");
    if (mode === "write" || mode === "split") {
      view.scrollLead = "source";
      if (!area.readOnly) { area.focus({ preventScroll: true }); area.setSelectionRange(heading.offset, heading.offset); }
      if (geometry.sourceTops[index] != null) area.scrollTop = Math.min(geometry.sourceMax, geometry.sourceTops[index]);
      syncNoteScroll("source");
    } else if (geometry.previewTops[index] != null) {
      view.scrollLead = "preview";
      pane.scrollTop = Math.max(0, Math.min(geometry.previewMax, geometry.previewTops[index] - 12));
    }
    view.outlineCurrent = -2;
    renderNoteOutlineCurrent();
  }

  function noteImageEncodings(bitmap, width) {
    const scale = Math.min(1, width / bitmap.width);
    const canvas = document.createElement("canvas");
    canvas.width = Math.max(1, Math.round(bitmap.width * scale));
    canvas.height = Math.max(1, Math.round(bitmap.height * scale));
    canvas.getContext("2d").drawImage(bitmap, 0, 0, canvas.width, canvas.height);
    return [canvas.toDataURL("image/png"), canvas.toDataURL("image/webp", 0.85), canvas.toDataURL("image/jpeg", 0.82)];
  }

  async function noteImageDataUrl(file, budget) {
    const original = await new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => resolve(String(reader.result || ""));
      reader.onerror = () => reject(reader.error);
      reader.readAsDataURL(file);
    });
    const bitmap = await createImageBitmap(file);
    try {
      if (/^data:image\/(?:png|jpeg|gif|webp);base64,/.test(original) && original.length <= budget && bitmap.width <= NOTE_IMAGE_MAX_WIDTH) return original;
      for (const width of [NOTE_IMAGE_MAX_WIDTH, 1200, 900, 600]) {
        const fits = noteImageEncodings(bitmap, width).filter((url) => /^data:image\/(?:png|jpeg|webp);base64,/.test(url) && url.length <= budget);
        if (fits.length) return fits.reduce((best, url) => url.length < best.length ? url : best);
      }
      return "";
    } finally { bitmap.close?.(); }
  }

  async function pasteNoteImage(event) {
    const area = event.target;
    const files = [...(event.clipboardData?.files || [])].filter((file) => file.type.startsWith("image/"));
    if (!files.length || area.readOnly || (event.clipboardData.getData("text/plain") || "").trim()) return;
    event.preventDefault();
    const main = $("[data-note-markdown]");
    for (const file of files) {
      const budget = Math.min(NOTE_IMAGE_MAX_CHARS, NOTE_MARKDOWN_MAX - main.value.length - 64);
      if (budget < 4096) { noteNotice("main", NOTE_ERRORS.note_full, "bad"); return; }
      let url = "";
      try { url = await noteImageDataUrl(file, budget); } catch (_) { noteNotice("main", NOTE_ERRORS.image_unreadable, "bad"); return; }
      if (!url) { noteNotice("main", NOTE_ERRORS.image_too_large, "bad"); return; }
      const target = area.isConnected ? area : main;
      const { selectionStart: start, selectionEnd: end, value } = target;
      const text = `${start > 0 && value[start - 1] !== "\n" ? "\n" : ""}![image](${url})${end < value.length && value[end] !== "\n" ? "\n" : ""}`;
      target.focus({ preventScroll: true });
      if (!document.execCommand("insertText", false, text)) {
        target.setRangeText(text, start, end, "end");
        target.dispatchEvent(new Event("input", { bubbles: true }));
      }
    }
  }

  function noteFollowPreviewElement(preview, lines, index) {
    for (let line = index; line >= 0 && line > index - 20; line -= 1) {
      const needle = String(lines[line] || "").replace(/[*_`#>~]|\[|\]\([^)]*\)|^\s*[-+]\s+(\[[ xX]\]\s+)?|^\s*\d+\.\s+/g, "").trim().slice(0, 60);
      if (!needle) continue;
      const walker = document.createTreeWalker(preview, NodeFilter.SHOW_TEXT);
      for (let node = walker.nextNode(); node; node = walker.nextNode()) {
        if (node.nodeValue.includes(needle)) return node.parentElement.closest("p, li, h1, h2, h3, h4, h5, h6, pre, blockquote, td, th, figure") || node.parentElement;
      }
    }
    return null;
  }

  function showNoteFollowTag(top, left) {
    const view = state.notesView, tag = $("[data-note-follow-tag]"), body = $(".notes-body");
    if (!tag || !body) return;
    const box = body.getBoundingClientRect();
    tag.textContent = view.follow;
    tag.style.background = noteAvatarColor(view.follow);
    tag.style.top = `${Math.max(0, Math.min(box.height - 20, top - box.top - 20))}px`;
    tag.style.left = `${Math.max(4, left - box.left)}px`;
    tag.hidden = false;
    clearTimeout(view.followTagTimer);
    view.followTagTimer = setTimeout(() => { tag.hidden = true; }, 3000);
  }

  function clearNoteFollowMarks() {
    document.querySelectorAll("[data-view=\"notes\"] [data-follow-name]").forEach((node) => {
      node.removeAttribute("data-follow-name");
      node.style.removeProperty("--follow-color");
    });
  }

  function noteFollowTo(line, column = 0) {
    const view = state.notesView, area = $("[data-note-markdown]");
    if (!area || !view.follow || !(line > 0)) return;
    const mode = $("[data-note-work]")?.dataset.mode || "split";
    const lines = area.value.split("\n");
    const index = Math.max(0, Math.min(lines.length - 1, line - 1));
    clearNoteFollowMarks();
    let tag = null;
    if (mode === "write" || mode === "split") {
      const pane = $("[data-note-write-pane]");
      const y = noteTextareaTop(area, noteLineOffset(lines, index) + Math.min(column, lines[index].length));
      if (area.scrollHeight > area.clientHeight) area.scrollTop = Math.max(0, y - area.clientHeight / 3);
      const inPane = area.getBoundingClientRect().top - pane.getBoundingClientRect().top + pane.scrollTop + y - area.scrollTop;
      pane.scrollTop = Math.max(0, inPane - pane.clientHeight / 3);
      const rect = area.getBoundingClientRect();
      tag = [rect.top + y - area.scrollTop, rect.left];
    }
    if (mode !== "write") {
      const pane = $("[data-note-preview-pane]"), preview = $("[data-note-preview]");
      let target = null;
      if (mode === "live") {
        for (const child of preview.children) if (child.matches(".notes-live-block") && Number(child.dataset.liveLine) <= index) target = child;
      } else target = noteFollowPreviewElement(preview, lines, index);
      if (target) {
        const inPane = target.getBoundingClientRect().top - pane.getBoundingClientRect().top + pane.scrollTop;
        pane.scrollTop = Math.max(0, inPane - pane.clientHeight / 3);
        target.dataset.followName = view.follow;
        target.style.setProperty("--follow-color", noteAvatarColor(view.follow));
        if (!tag) { const rect = target.getBoundingClientRect(); tag = [rect.top, rect.left]; }
      }
    }
    if (tag) showNoteFollowTag(tag[0], tag[1]);
  }

  function renderNoteFollow() {
    const view = state.notesView, box = $("[data-note-follow]");
    if (!box) return;
    box.hidden = !view.follow;
    $("[data-note-follow-label]").textContent = view.follow ? `Following ${view.follow}` : "";
  }

  function setNoteFollow(name) {
    const view = state.notesView;
    view.follow = name || "";
    renderNoteFollow();
    if (!view.follow) {
      clearNoteFollowMarks();
      const tag = $("[data-note-follow-tag]"); if (tag) tag.hidden = true;
      return;
    }
    const peer = [...view.peers.values()].filter((item) => item.name === view.follow && item.line > 0)
      .sort((a, b) => (b.movedAt || 0) - (a.movedAt || 0))[0];
    if (peer) noteFollowTo(peer.line, peer.column);
  }

  function noteChatSeenMap() {
    try { return JSON.parse(localStorage.getItem(NOTE_CHAT_SEEN_KEY) || "{}") || {}; } catch (_) { return {}; }
  }

  function markNoteChatSeen() {
    const view = state.notesView, note = view.selected, messages = view.chat?.messages || [];
    if (!note || !messages.length) return;
    const latest = Number(messages[messages.length - 1].createdAt || 0), seen = noteChatSeenMap();
    if (Number(seen[note.id] || 0) >= latest) return;
    delete seen[note.id];
    seen[note.id] = latest;
    const ids = Object.keys(seen);
    for (const id of ids.slice(0, Math.max(0, ids.length - 200))) delete seen[id];
    try { localStorage.setItem(NOTE_CHAT_SEEN_KEY, JSON.stringify(seen)); } catch (_) {}
  }

  function noteChatUnread() {
    const view = state.notesView, note = view.selected;
    if (!note || !view.chat) return 0;
    const seen = Number(noteChatSeenMap()[note.id] || 0), me = noteMe();
    return view.chat.messages.filter((message) => message.author !== me && Number(message.createdAt || 0) > seen).length;
  }

  function noteChatVisible() { return state.notesView.commentsOpen && state.notesView.panelTab === "chat"; }

  function applyNoteChat(noteId, payload) {
    const view = state.notesView;
    if (view.selected?.id !== noteId) return;
    view.chat = { messages: payload.messages || [], canModerate: Boolean(payload.canModerate) };
    renderNoteChat();
  }

  async function loadNoteChat() {
    const view = state.notesView, note = view.selected;
    if (!note || !state.session?.sessionToken) return;
    const generation = ++view.chatGeneration;
    try {
      const payload = await noteRequest(`/api/notes/${note.id}/chat`);
      if (generation === view.chatGeneration) applyNoteChat(note.id, payload);
    } catch (error) {
      if (generation === view.chatGeneration && view.selected?.id === note.id) noteNotice("chat", error.message, "bad");
    }
  }

  function scheduleNoteChat() {
    const view = state.notesView;
    clearTimeout(view.chatTimer);
    view.chatTimer = setTimeout(() => void loadNoteChat(), NOTE_COMMENTS_REFRESH_MS);
  }

  async function sendNoteChat(path, method, body) {
    const view = state.notesView, note = view.selected;
    if (!note) return null;
    try {
      const payload = await noteRequest(`/api/notes/${note.id}/chat${path}`, method, body);
      view.chatGeneration++;
      view.chatScroll = method === "POST";
      applyNoteChat(note.id, payload);
      noteNotice("chat", "");
      return payload;
    } catch (error) {
      noteNotice("chat", error.message, "bad");
      return null;
    }
  }

  async function submitNoteChat(form) {
    const view = state.notesView, input = form?.querySelector("[data-note-chat-input]");
    const body = String(input?.value || "").trim();
    if (!body || view.chatSending) return;
    view.chatSending = true;
    try {
      if (await sendNoteChat("", "POST", { body })) { input.value = ""; growNoteChatInput(input); }
    } finally { view.chatSending = false; }
  }

  function growNoteChatInput(input) {
    input.style.height = "auto";
    input.style.height = `${Math.min(input.scrollHeight + 2, 128)}px`;
  }

  function noteChatHtml(message) {
    const view = state.notesView;
    const mine = Boolean(message.author) && message.author === noteMe();
    const tools = mine || view.chat?.canModerate
      ? `<span class="notes-comment-tools"><button type="button" class="notes-icon-btn" data-note-chat-delete="${escapeHtml(message.id)}" title="Delete" aria-label="Delete message">${noteIcon("trash-2")}</button></span>` : "";
    return `<li class="notes-chat-msg" data-note-chat-message="${escapeHtml(message.id)}">
        <span class="notes-avatar" style="background:${noteAvatarColor(message.author)}">${escapeHtml(noteInitials(message.author))}</span>
        <div class="notes-chat-head">
          <span class="notes-comment-who">${mine ? "You" : escapeHtml(message.author || "Someone")}</span>
          <time class="notes-comment-time" title="${escapeHtml(message.createdAt ? new Date(message.createdAt).toLocaleString() : "")}">${escapeHtml(message.createdAt ? relativeTimeLabel(message.createdAt) : "")}</time>
          ${tools}
        </div>
        <div class="notes-chat-body">${escapeHtml(message.body || "")}</div>
      </li>`;
  }

  function renderNoteChat() {
    const view = state.notesView, visible = noteChatVisible();
    if (visible) markNoteChatSeen();
    const unread = visible ? 0 : noteChatUnread();
    for (const badge of [$("[data-note-chat-badge]"), $("[data-note-chat-unread]")]) {
      if (!badge) continue;
      badge.hidden = !unread;
      badge.textContent = unread > 99 ? "99+" : String(unread);
    }
    $("[data-note-chat-toggle]")?.setAttribute("title", unread ? `Chat · ${noteCountLabel(unread, "new message", "new messages")}` : "Chat");
    const list = $("[data-note-chat-list]");
    if (!list || !visible) return;
    const atBottom = list.scrollHeight - list.scrollTop - list.clientHeight < 40;
    const messages = view.chat?.messages || [];
    list.innerHTML = messages.map(noteChatHtml).join("")
      || `<li class="notes-comments-empty">${view.chat ? "No messages yet. Say hello to everyone in this note." : "Loading chat…"}</li>`;
    if (atBottom || view.chatScroll) { list.scrollTop = list.scrollHeight; view.chatScroll = false; }
    refreshNoteIcons();
  }

  function renderNotePanelToggles() {
    const comments = noteSideShows("comments"), chat = noteSideShows("chat");
    const activity = noteSideShows("activity") || noteSideShows("history");
    const commentsToggle = $("[data-note-comments-toggle]"), chatToggle = $("[data-note-chat-toggle]"), activityToggle = $("[data-note-activity-toggle]");
    commentsToggle?.setAttribute("aria-pressed", comments ? "true" : "false");
    commentsToggle?.setAttribute("aria-label", comments ? "Hide comments" : "Show comments");
    chatToggle?.setAttribute("aria-pressed", chat ? "true" : "false");
    chatToggle?.setAttribute("aria-label", chat ? "Hide chat" : "Show chat");
    activityToggle?.setAttribute("aria-pressed", activity ? "true" : "false");
    activityToggle?.setAttribute("aria-label", activity ? "Hide activity" : "Show activity");
  }

  function setNotePanelTab(tab) {
    const view = state.notesView;
    const previous = view.panelTab;
    view.panelTab = NOTE_SIDE_TABS.includes(tab) ? tab : "comments";
    document.querySelectorAll("[data-note-panel-tab]").forEach((button) =>
      button.setAttribute("aria-selected", button.dataset.notePanelTab === view.panelTab ? "true" : "false"));
    document.querySelectorAll("[data-note-panel]").forEach((panel) => { panel.hidden = panel.dataset.notePanel !== view.panelTab; });
    const general = $("[data-note-comment-general]");
    if (general) general.hidden = view.panelTab !== "comments";
    if (view.panelTab !== "comments") hideNoteMentions();
    renderNotePanelToggles();
    view.chatScroll = view.panelTab === "chat";
    renderNoteChat();
    renderNoteComments();
    if (view.panelTab === "activity") renderNoteActivity();
    if (view.panelTab === "history" && (previous !== "history" || !view.commentsOpen)) void loadNoteVersions();
    if (view.commentsOpen) rememberNoteSide();
  }

  function openNotePanel(tab) {
    setNotePanelTab(tab);
    setNoteCommentsOpen(true);
    if (tab === "chat") $("[data-note-chat-input]")?.focus();
  }

  function toggleNotePanel(tab) {
    if (noteSideShows(tab)) setNoteCommentsOpen(false);
    else openNotePanel(tab);
  }

  function wireNoteLiveAndChat() {
    const view = state.notesView, preview = $("[data-note-preview]"), heading = $("[data-note-preview-title]");
    preview?.addEventListener("click", noteLiveClick);
    preview?.addEventListener("keydown", noteLiveKeydown);
    preview?.addEventListener("input", (event) => { if (event.target.matches("[data-note-live-input]")) noteLiveInput(); });
    for (const type of ["keyup", "select"]) {
      preview?.addEventListener(type, (event) => {
        if (!event.target.matches("[data-note-live-input]")) return;
        view.selectionSource = "live"; sendNoteCursor();
      });
    }
    preview?.addEventListener("focusout", (event) => {
      const area = event.target;
      if (!area.matches?.("[data-note-live-input]")) return;
      setTimeout(() => { if (view.live?.area === area && document.activeElement !== area) endNoteLiveEdit(); }, 0);
    });
    heading?.addEventListener("input", () => {
      if (!noteLiveMode()) return;
      $("[data-note-title]").value = heading.textContent.replace(/\s+/g, " ").slice(0, 200);
      onNoteEdited();
    });
    heading?.addEventListener("keydown", (event) => {
      if (!noteLiveMode() || event.key !== "Enter") return;
      event.preventDefault();
      const first = [...preview.children].find((child) => child.matches(".notes-live-block"));
      if (first) startNoteLiveEdit(first, 0); else startNoteLiveBlockAtEnd();
    });
    heading?.addEventListener("paste", (event) => {
      if (!noteLiveMode()) return;
      event.preventDefault();
      document.execCommand("insertText", false, (event.clipboardData?.getData("text/plain") || "").replace(/\s+/g, " "));
    });
    const area = $("[data-note-markdown]"), pane = $("[data-note-preview-pane]");
    for (const type of ["keyup", "mouseup", "select"]) area?.addEventListener(type, sendNoteCursor);
    area?.addEventListener("paste", (event) => void pasteNoteImage(event));
    preview?.addEventListener("paste", (event) => { if (event.target.matches("[data-note-live-input]")) void pasteNoteImage(event); });
    area?.addEventListener("scroll", () => { syncNoteScroll("source"); renderNoteOutlineCurrent(); }, { passive: true });
    pane?.addEventListener("scroll", () => { syncNoteScroll("preview"); renderNoteOutlineCurrent(); }, { passive: true });
    preview?.addEventListener("load", forgetNoteScrollGeometry, true);
    for (const type of ["mouseenter", "wheel", "pointerdown", "touchstart", "focusin", "input"]) {
      area?.addEventListener(type, () => { view.scrollLead = "source"; }, { passive: true });
      pane?.addEventListener(type, () => { view.scrollLead = "preview"; }, { passive: true });
    }
    if (window.ResizeObserver) {
      const resized = new ResizeObserver(forgetNoteScrollGeometry);
      for (const node of [area, pane, preview]) if (node) resized.observe(node);
    }
    try { view.outlineOpen = localStorage.getItem(NOTE_OUTLINE_KEY) === "open"; } catch (_) { view.outlineOpen = false; }
    renderNoteOutlineOpen();
    $("[data-note-outline-toggle]")?.addEventListener("click", () => setNoteOutlineOpen(!view.outlineOpen));
    $("[data-note-outline]")?.addEventListener("click", (event) => {
      const entry = event.target.closest("[data-note-outline-entry]");
      if (entry) jumpToNoteHeading(Number(entry.dataset.noteOutlineEntry));
    });
    $("[data-note-chat-toggle]")?.addEventListener("click", () => toggleNotePanel("chat"));
    document.querySelectorAll("[data-note-panel-tab]").forEach((button) =>
      button.addEventListener("click", () => setNotePanelTab(button.dataset.notePanelTab)));
    const form = $("[data-note-chat-form]"), input = $("[data-note-chat-input]");
    form?.addEventListener("submit", (event) => { event.preventDefault(); void submitNoteChat(form); });
    input?.addEventListener("input", () => growNoteChatInput(input));
    input?.addEventListener("keydown", (event) => {
      if (event.key === "Enter" && !event.shiftKey && !event.isComposing) { event.preventDefault(); void submitNoteChat(form); }
    });
    $("[data-note-chat-list]")?.addEventListener("click", (event) => {
      const remove = event.target.closest("[data-note-chat-delete]");
      if (remove && window.confirm("Delete this message?")) void sendNoteChat(`/${remove.dataset.noteChatDelete}`, "DELETE", {});
    });
    $("[data-note-people-panel]")?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-note-follow-person]");
      if (!button) return;
      const name = button.dataset.noteFollowPerson;
      closeNotePopovers();
      setNoteFollow(view.follow === name ? "" : name);
    });
    $("[data-note-follow-stop]")?.addEventListener("click", () => setNoteFollow(""));
  }

  function closeNotePopovers() {
    for (const [panel, trigger] of [["[data-note-share-panel]", "[data-note-share-open]"], ["[data-note-menu]", "[data-note-menu-open]"], ["[data-note-people-panel]", "[data-note-presence]"], ["[data-note-emoji-panel]", "[data-note-emoji-open]"], ["[data-note-page-share-panel]", "[data-note-page-share-open]"]]) {
      const element = $(panel); if (element) element.hidden = true;
      $(trigger)?.setAttribute("aria-expanded", "false");
    }
  }

  function toggleNotePopover(panel, trigger) {
    const element = $(panel), open = element.hidden;
    closeNotePopovers();
    element.hidden = !open;
    $(trigger).setAttribute("aria-expanded", open ? "true" : "false");
    if (open) element.querySelector("input, button:not([disabled]), a")?.focus();
  }

  function setNoteMode(mode) {
    state.notesView.mode = mode;
    try { localStorage.setItem(NOTE_MODE_KEY, mode); } catch (_) {}
    endNoteLiveEdit(false);
    selectNoteImage(null);
    closeNoteImagePanel();
    state.notesView.pagesKey = "";
    if (state.notesView.selected) { renderNoteMode(state.notesView.selected); renderNotePreview(); }
  }

  function setNoteFilter(filter) {
    state.notesView.filter = filter;
    document.querySelectorAll("[data-note-filter]").forEach((button) =>
      button.setAttribute("aria-pressed", button.dataset.noteFilter === filter ? "true" : "false"));
    renderNoteList();
  }

  function showNoteEditorOnMobile() { $("[data-note-app]").dataset.mobile = "editor"; }

  // "owner/repo#12", or an issue / pull / discussion URL from this site.
  function parseNoteLink(value, fallbackKind) {
    const text = String(value || "").trim();
    const kinds = { issues: "issue", issue: "issue", pull: "pull", pulls: "pull", discussions: "discussion", discussion: "discussion" };
    let match = text.match(/^([\w.-]+)\/([\w.-]+)#(\d+)$/);
    if (match) return { owner: match[1], repo: match[2], kind: fallbackKind, number: Number(match[3]) };
    try {
      const url = new URL(text, location.origin);
      const parts = url.pathname.split("/").filter(Boolean);
      match = url.pathname.match(/^\/([\w.-]+)\/([\w.-]+)\/(issues|pull|pulls|discussions)\/(\d+)/);
      if (match) return { owner: match[1], repo: match[2], kind: kinds[match[3]], number: Number(match[4]) };
      const tab = url.searchParams.get("tab"), number = Number(url.hash.slice(1));
      if (parts.length === 2 && kinds[tab] && number) return { owner: parts[0], repo: parts[1], kind: kinds[tab], number };
    } catch (_) {}
    return null;
  }

  async function addNoteLink(form) {
    const note = state.notesView.selected; if (!note) return;
    const input = form.querySelector("[data-note-link-input]");
    const link = parseNoteLink(input.value, form.querySelector("[data-note-link-kind]").value);
    if (!link) { noteNotice("links", NOTE_ERRORS.invalid_link, "bad"); input.focus(); return; }
    try {
      const payload = await noteRequest(`/api/notes/${note.id}/links`, "POST", link);
      storeNote({ ...state.notesView.selected, links: payload.links || [] });
      scheduleNoteActivity();
      state.notesView.linking = false;
      renderNoteLinks(state.notesView.selected);
      noteNotice("links", `Linked ${link.repo}#${link.number}`);
    } catch (error) { noteNotice("links", error.message, "bad"); input.focus(); }
  }

  function startNoteLink() {
    closeNotePopovers();
    if (["preview", "live"].includes($("[data-note-work]").dataset.mode)) setNoteMode("split");
    state.notesView.linking = true;
    renderNoteLinks(state.notesView.selected);
    $("[data-note-link-input]")?.focus();
  }

  function noteShareScope() { return state.notesView.shareScope || "note"; }

  function noteShareQuery() {
    return ($(`[data-${noteShareScope()}-share-name]`)?.value || "").trim().replace(/^@/, "").toLowerCase();
  }

  function loadNoteShareOrgs() {
    const view = state.notesView;
    if (!view.shareOrgs) {
      view.shareOrgs = noteRequest("/api/orgs")
        .then((payload) => (payload.orgs || []).map((org) => String(org?.name || "").trim().toLowerCase()).filter(Boolean))
        .catch(() => { view.shareOrgs = null; return []; });
    }
    return view.shareOrgs;
  }

  function renderNoteShareSuggestions(items, index = items.length ? 0 : -1) {
    const view = state.notesView, scope = noteShareScope();
    const list = $(`[data-${scope}-share-suggestions]`), input = $(`[data-${scope}-share-name]`);
    if (!list || !input) return;
    view.shareSuggestions = items; view.shareSuggestionIndex = index;
    const open = items.length > 0 && document.activeElement === input;
    list.innerHTML = open ? items.map((item, position) => `
      <button type="button" role="option" id="${scope}-share-option-${position}" tabindex="-1" data-${scope}-share-option="${position}" aria-selected="${position === index ? "true" : "false"}">
        <span class="notes-avatar" data-type="${escapeHtml(item.type)}" style="background:${noteAvatarColor(item.name)}">${escapeHtml(noteInitials(item.name))}</span>
        <span class="notes-person-name">${escapeHtml(item.name)}</span>
        <span class="notes-person-hint">${item.type === "organization" ? "Organization" : "User"}</span>
      </button>`).join("") : "";
    list.hidden = !open;
    input.setAttribute("aria-expanded", open ? "true" : "false");
    if (open && index >= 0) {
      input.setAttribute("aria-activedescendant", `${scope}-share-option-${index}`);
      list.querySelector(`#${scope}-share-option-${index}`)?.scrollIntoView({ block: "nearest" });
    } else input.removeAttribute("aria-activedescendant");
  }

  function searchNoteShareCandidates() {
    const view = state.notesView, query = noteShareQuery();
    const generation = ++view.shareGeneration;
    clearTimeout(view.shareTimer);
    if (!query) { renderNoteShareSuggestions([]); return; }
    view.shareTimer = setTimeout(async () => {
      const [users, orgs] = await Promise.all([
        noteRequest(`/api/chat/direct-messages/users?query=${encodeURIComponent(query)}`)
          .then((payload) => (payload.users || []).map((user) => String(user?.name || "").trim().toLowerCase()))
          .catch(() => []),
        loadNoteShareOrgs(),
      ]);
      if (generation !== view.shareGeneration) return;
      const owner = String(state.session?.nodeName || "").toLowerCase();
      const current = noteShareScope() === "note-page" ? view.page?.page?.shares : view.selected?.shares;
      const taken = new Set((current || []).map((share) => `${share.type}:${String(share.name).toLowerCase()}`));
      const items = [
        ...users.filter((name) => name && name !== owner).map((name) => ({ type: "user", name })),
        ...orgs.filter((name) => name.includes(query)).map((name) => ({ type: "organization", name })),
      ].filter((item, position, all) => !taken.has(`${item.type}:${item.name}`)
        && all.findIndex((other) => other.type === item.type && other.name === item.name) === position)
        .sort((left, right) => Number(right.name.startsWith(query)) - Number(left.name.startsWith(query)))
        .slice(0, 8);
      renderNoteShareSuggestions(items);
    }, 150);
  }

  function chooseNoteShareCandidate(item) {
    const scope = noteShareScope();
    const input = $(`[data-${scope}-share-name]`); if (!input || !item) return;
    input.value = item.name;
    $(`[data-${scope}-share-type]`).value = item.type;
    state.notesView.shareGeneration++;
    clearTimeout(state.notesView.shareTimer);
    renderNoteShareSuggestions([]);
    input.focus();
  }

  function wireNoteShareField(scope) {
    const view = state.notesView;
    const shareName = $(`[data-${scope}-share-name]`);
    const suggestions = $(`[data-${scope}-share-suggestions]`);
    shareName?.addEventListener("input", () => { view.shareScope = scope; searchNoteShareCandidates(); });
    shareName?.addEventListener("focus", () => { view.shareScope = scope; if (noteShareQuery()) searchNoteShareCandidates(); });
    shareName?.addEventListener("blur", () => { view.shareGeneration++; renderNoteShareSuggestions([]); });
    shareName?.addEventListener("keydown", (event) => {
      view.shareScope = scope;
      const items = view.shareSuggestions || [];
      if (suggestions.hidden || !items.length) return;
      if (event.key === "ArrowDown" || event.key === "ArrowUp") {
        event.preventDefault();
        const step = event.key === "ArrowDown" ? 1 : -1;
        renderNoteShareSuggestions(items, (view.shareSuggestionIndex + step + items.length) % items.length);
      } else if (event.key === "Escape") {
        event.stopPropagation(); view.shareGeneration++; renderNoteShareSuggestions([]);
      } else if ((event.key === "Enter" || event.key === "Tab") && view.shareSuggestionIndex >= 0) {
        const item = items[view.shareSuggestionIndex];
        const exact = item.name === noteShareQuery() && item.type === $(`[data-${scope}-share-type]`).value;
        if (event.key === "Tab" || !exact) event.preventDefault();
        chooseNoteShareCandidate(item);
      }
    });
    suggestions?.addEventListener("mousedown", (event) => event.preventDefault());
    suggestions?.addEventListener("click", (event) => {
      const option = event.target.closest(`[data-${scope}-share-option]`); if (!option) return;
      view.shareScope = scope;
      chooseNoteShareCandidate(view.shareSuggestions[Number(option.getAttribute(`data-${scope}-share-option`))]);
    });
  }

  async function updateNoteShare(body, method, message) {
    const note = state.notesView.selected; if (!note) return false;
    try {
      const payload = await noteRequest(`/api/notes/${note.id}/shares`, method, body);
      storeNote({ ...state.notesView.selected, shares: payload.shares || [] });
      renderNoteEditor(state.notesView.selected, { keepDraft: true });
      noteNotice("share", message);
      scheduleNoteActivity();
      return true;
    } catch (error) { noteNotice("share", error.message, "bad"); return false; }
  }

  function formatNoteSelection(kind) {
    const area = state.notesView.live?.area || $("[data-note-markdown]"); if (!area || area.readOnly) return;
    const { selectionStart: start, selectionEnd: end, value } = area;
    const selected = value.slice(start, end);
    const wraps = { bold: ["**", "**", "bold text"], italic: ["_", "_", "italic text"], code: ["`", "`", "code"], link: ["[", "](https://)", "link text"] };
    let next, cursorStart, cursorEnd;
    if (wraps[kind]) {
      const [before, after, placeholder] = wraps[kind];
      const body = selected || placeholder;
      next = value.slice(0, start) + before + body + after + value.slice(end);
      cursorStart = start + before.length; cursorEnd = cursorStart + body.length;
    } else {
      const prefix = { heading: "## ", list: "- ", task: "- [ ] " }[kind];
      const lineStart = value.lastIndexOf("\n", start - 1) + 1;
      next = value.slice(0, lineStart) + prefix + value.slice(lineStart);
      cursorStart = start + prefix.length; cursorEnd = end + prefix.length;
    }
    area.value = next;
    area.focus(); area.setSelectionRange(cursorStart, cursorEnd);
    area.dispatchEvent(new Event("input", { bubbles: true }));
  }

  function noteConversionTitle(title, markdown) {
    const trimmed = String(title || "").trim();
    if (trimmed) return trimmed.slice(0, 160);
    const line = String(markdown || "").split("\n").map((text) => text.trim()).find(Boolean) || "";
    return (line.replace(/^#+/, "").trim() || "Untitled note").slice(0, 160);
  }

  function noteConversionDetails(title, markdown, publicUrl, limit = NOTE_TASK_DETAILS_MAX) {
    let footer = `\n\n---\nConverted from the note "${noteConversionTitle(title, markdown)}"`;
    if (publicUrl) footer += ` (published at ${publicUrl})`;
    const text = String(markdown || ""), room = Math.max(0, limit - footer.length);
    return (text.length > room ? `${text.slice(0, Math.max(0, room - 1))}…` : text) + footer;
  }

  function noteConversionSource() {
    const note = state.notesView.selected; if (!note) return null;
    return {
      title: $("[data-note-title]").value,
      markdown: $("[data-note-markdown]").value,
      link: note.visibility === "public" ? `${location.origin}/notes/${note.id}` : "",
    };
  }

  function setNoteConverting(busy) {
    state.notesView.converting = busy;
    document.querySelectorAll("[data-note-to-task], [data-note-to-issue]").forEach((item) => { item.disabled = busy; });
  }

  async function convertNoteToTask() {
    closeNotePopovers();
    const source = noteConversionSource(); if (!source || state.notesView.converting) return;
    noteNotice("");
    setNoteConverting(true);
    try {
      await noteRequest("/api/tasks", "POST", {
        title: noteConversionTitle(source.title, source.markdown),
        details: noteConversionDetails(source.title, source.markdown, source.link),
        department: "general", destination: "department", assigneeKind: "unassigned",
      });
      noteNotice("main", "Task created from this note");
      void recordNoteActivity("task");
    } catch (error) {
      noteNotice("main", NOTE_TASK_ERRORS[error.payload?.error] || "The task was not created.", "bad");
    } finally { setNoteConverting(false); }
  }

  function askNoteIssueRepository() {
    closeNotePopovers();
    if (!state.notesView.selected || state.notesView.converting) return;
    const input = $("[data-note-issue-repo]");
    input.value = "";
    noteNotice("");
    $("[data-note-issue-dialog]").showModal();
    input.focus();
  }

  async function convertNoteToIssue(repository) {
    const source = noteConversionSource(); if (!source || state.notesView.converting) return;
    if (!state.session?.sessionToken) { noteNotice("main", "Sign in on this device to file issues.", "bad"); return; }
    setNoteConverting(true);
    try {
      const filed = await submitWebIssue(repository, noteConversionTitle(source.title, source.markdown),
        noteConversionDetails(source.title, source.markdown, source.link, NOTE_ISSUE_BODY_MAX));
      noteNotice("main", "Issue filed from this note");
      const number = Number(filed?.number || filed?.event?.number || 0);
      void recordNoteActivity("issue", number ? { owner: repository.owner, repo: repository.name, number } : null);
    } catch (_) {
      noteNotice("main", "The issue was not filed.", "bad");
    } finally { setNoteConverting(false); }
  }

  function noteCommentsOpenCount() {
    return (state.notesView.comments?.comments || []).filter((comment) => !comment.parentId && !comment.resolvedAt).length;
  }

  function applyNoteComments(noteId, payload) {
    const view = state.notesView;
    if (view.selected?.id !== noteId) return;
    view.comments = { comments: payload.comments || [], reactions: payload.reactions || [], people: payload.people || [], canResolve: Boolean(payload.canResolve) };
    if (view.pendingComment && view.comments.comments.some((comment) => comment.id === view.pendingComment)) {
      const wanted = view.comments.comments.find((comment) => comment.id === view.pendingComment);
      view.commentFocus = wanted.parentId || wanted.id;
      view.pendingComment = "";
      openNotePanel("comments");
      requestAnimationFrame(() => $(`[data-note-thread="${view.commentFocus}"]`)?.scrollIntoView({ block: "nearest" }));
    }
    renderNoteComments();
    renderNotePreview();
  }

  async function loadNoteComments() {
    const view = state.notesView, note = view.selected;
    if (!note || !state.session?.sessionToken) return;
    const generation = ++view.commentsGeneration;
    try {
      const payload = await noteRequest(`/api/notes/${note.id}/comments`);
      if (generation === view.commentsGeneration) applyNoteComments(note.id, payload);
    } catch (error) {
      if (generation === view.commentsGeneration && view.selected?.id === note.id) noteNotice("comments", error.message, "bad");
    }
  }

  function scheduleNoteComments() {
    const view = state.notesView;
    clearTimeout(view.commentsTimer);
    view.commentsTimer = setTimeout(() => void loadNoteComments(), NOTE_COMMENTS_REFRESH_MS);
  }

  async function sendNoteComment(path, method, body, success = "") {
    const view = state.notesView, note = view.selected;
    if (!note) return null;
    try {
      const payload = await noteRequest(`/api/notes/${note.id}/comments${path}`, method, body);
      view.commentsGeneration++;
      applyNoteComments(note.id, payload);
      scheduleNoteActivity();
      noteNotice("comments", success);
      return payload;
    } catch (error) {
      noteNotice("comments", error.message, "bad");
      return null;
    }
  }

  function noteLocateQuote(markdown, comment) {
    const quote = String(comment.quote || "");
    if (!quote) return null;
    const prefix = String(comment.prefix || ""), suffix = String(comment.suffix || "");
    let best = null;
    for (let at = markdown.indexOf(quote); at !== -1; at = markdown.indexOf(quote, at + 1)) {
      const score = (prefix && markdown.slice(Math.max(0, at - prefix.length), at) === prefix ? 2 : 0)
        + (suffix && markdown.slice(at + quote.length, at + quote.length + suffix.length) === suffix ? 2 : 0);
      const distance = Math.abs(at - Number(comment.start || 0));
      if (!best || score > best.score || (score === best.score && distance < best.distance)) {
        best = { start: at, end: at + quote.length, score, distance };
      }
    }
    return best;
  }

  function noteCommentThreads() {
    const all = state.notesView.comments?.comments || [];
    const markdown = $("[data-note-markdown]")?.value || "";
    const threads = all.filter((comment) => !comment.parentId).map((comment) => ({
      ...comment,
      replies: all.filter((reply) => reply.parentId === comment.id),
      anchor: noteLocateQuote(markdown, comment),
    }));
    const rank = (thread) => !thread.quote ? 0 : thread.anchor ? 1 : 2;
    const order = (a, b) => rank(a) - rank(b) || (a.anchor?.start || 0) - (b.anchor?.start || 0) || a.createdAt - b.createdAt;
    return {
      open: threads.filter((thread) => !thread.resolvedAt).sort(order),
      resolved: threads.filter((thread) => thread.resolvedAt).sort(order),
    };
  }

  function noteCommentBodyHtml(comment) {
    const mentioned = new Set((comment.mentions || []).map((name) => String(name).toLowerCase()));
    return escapeHtml(comment.body || "")
      .replace(/(^|[^\w@])@([a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)/gi, (whole, lead, name) =>
        mentioned.has(name.toLowerCase()) ? `${lead}<span class="notes-mention">@${name}</span>` : whole)
      .replace(/\n/g, "<br>");
  }

  function noteCommentHtml(comment) {
    const view = state.notesView;
    const mine = Boolean(comment.author) && comment.author === noteMe();
    const canDelete = mine || view.selected?.role === "owner";
    const tools = (mine ? `<button type="button" class="notes-icon-btn" data-note-comment-edit="${comment.id}" title="Edit" aria-label="Edit comment">${noteIcon("pencil")}</button>` : "")
      + (canDelete ? `<button type="button" class="notes-icon-btn" data-note-comment-delete="${comment.id}" title="Delete" aria-label="Delete comment">${noteIcon("trash-2")}</button>` : "");
    const body = view.commentEditing === comment.id
      ? `<form class="notes-comment-form" data-note-comment-edit-form="${comment.id}">
          <textarea data-note-comment-input rows="2" aria-label="Edit comment">${escapeHtml(comment.body || "")}</textarea>
          <div class="notes-comment-actions"><button type="button" class="notes-btn" data-note-comment-edit-cancel>Cancel</button><button type="submit" class="notes-btn notes-btn-primary">Save</button></div>
        </form>`
      : `<div class="notes-comment-body">${noteCommentBodyHtml(comment)}</div>`;
    return `<div class="notes-comment" data-note-comment="${comment.id}">
        <div class="notes-comment-head">
          <span class="notes-avatar" style="background:${noteAvatarColor(comment.author)}">${escapeHtml(noteInitials(comment.author))}</span>
          <span class="notes-comment-who">${mine ? "You" : escapeHtml(comment.author || "Someone")}</span>
          <time class="notes-comment-time" title="${escapeHtml(comment.createdAt ? new Date(comment.createdAt).toLocaleString() : "")}">${escapeHtml(comment.createdAt ? relativeTimeLabel(comment.createdAt) : "")}${comment.editedAt ? " · edited" : ""}</time>
          ${tools ? `<span class="notes-comment-tools">${tools}</span>` : ""}
        </div>
        ${body}
      </div>`;
  }

  function noteThreadHtml(thread) {
    const view = state.notesView;
    const canResolve = view.comments?.canResolve || thread.author === noteMe();
    const quote = thread.quote
      ? `<button type="button" class="notes-thread-quote" data-note-thread-jump="${thread.id}"${thread.anchor ? "" : ' data-detached="true" title="This text is no longer in the note"'}>${escapeHtml(thread.quote)}</button>`
      : `<span class="notes-thread-general">On the whole note</span>`;
    const resolve = canResolve
      ? `<button type="button" class="notes-icon-btn" data-note-thread-resolve="${thread.id}" title="${thread.resolvedAt ? "Reopen" : "Resolve"}" aria-label="${thread.resolvedAt ? "Reopen thread" : "Resolve thread"}">${noteIcon(thread.resolvedAt ? "undo-2" : "check")}</button>`
      : "";
    const footer = thread.resolvedAt
      ? `<p class="notes-thread-resolved">Resolved${thread.resolvedBy ? ` by ${escapeHtml(thread.resolvedBy === noteMe() ? "you" : thread.resolvedBy)}` : ""} ${escapeHtml(relativeTimeLabel(thread.resolvedAt))}</p>`
      : `<form class="notes-reply" data-note-reply-form="${thread.id}">
          <textarea data-note-comment-input rows="1" placeholder="Reply… use @ to mention" aria-label="Reply"></textarea>
          <button type="submit" class="notes-icon-btn" aria-label="Send reply">${noteIcon("send")}</button>
        </form>`;
    return `<li class="notes-thread" data-note-thread="${thread.id}"${view.commentFocus === thread.id ? ' data-focus="true"' : ""}>
        <div class="notes-thread-top">${quote}${resolve}</div>
        ${noteCommentHtml(thread)}
        ${thread.replies.map(noteCommentHtml).join("")}
        ${footer}
      </li>`;
  }

  function renderNoteComments() {
    const view = state.notesView;
    const count = noteCommentsOpenCount();
    const label = $("[data-note-comments-count]");
    if (label) label.textContent = count ? String(count) : "Comments";
    $("[data-note-comments-toggle]")?.setAttribute("title", count ? `${noteCountLabel(count, "open comment", "open comments")}` : "Comments");
    const panel = $("[data-note-comments]");
    if (!panel || panel.hidden) return;
    const kept = {};
    let focused = null;
    panel.querySelectorAll("form").forEach((form) => {
      const key = form.dataset.noteReplyForm ? `reply:${form.dataset.noteReplyForm}` : form.dataset.noteCommentEditForm ? `edit:${form.dataset.noteCommentEditForm}` : form.matches("[data-note-comment-new]") ? "new" : "";
      const input = form.querySelector("[data-note-comment-input]");
      if (!key || !input) return;
      kept[key] = input.value;
      if (document.activeElement === input) focused = { key, start: input.selectionStart, end: input.selectionEnd };
    });
    const composer = $("[data-note-comment-composer]");
    const draft = view.commentDraft;
    composer.innerHTML = draft ? `<form class="notes-comment-new" data-note-comment-new>
        <blockquote>${draft.quote ? escapeHtml(draft.quote) : "On the whole note"}</blockquote>
        <textarea data-note-comment-input rows="3" placeholder="Add a comment… use @ to mention" aria-label="New comment"></textarea>
        <div class="notes-comment-actions"><button type="button" class="notes-btn" data-note-comment-cancel>Cancel</button><button type="submit" class="notes-btn notes-btn-primary">Comment</button></div>
      </form>` : "";
    const { open, resolved } = noteCommentThreads();
    $("[data-note-comment-list]").innerHTML = open.map(noteThreadHtml).join("")
      || (view.comments ? `<li class="notes-comments-empty">${resolved.length ? "No open comments." : "No comments yet. Select text in the note and choose Comment."}</li>` : `<li class="notes-comments-empty">Loading comments…</li>`);
    const resolvedBox = $("[data-note-comment-resolved]");
    resolvedBox.hidden = !resolved.length;
    $("[data-note-comment-resolved-label]").textContent = `Resolved (${resolved.length})`;
    $("[data-note-comment-resolved-list]").innerHTML = resolved.map(noteThreadHtml).join("");
    panel.querySelectorAll("form").forEach((form) => {
      const key = form.dataset.noteReplyForm ? `reply:${form.dataset.noteReplyForm}` : form.dataset.noteCommentEditForm ? `edit:${form.dataset.noteCommentEditForm}` : form.matches("[data-note-comment-new]") ? "new" : "";
      const input = form.querySelector("[data-note-comment-input]");
      if (!key || !input) return;
      if (key in kept) input.value = kept[key];
      if (focused?.key === key) { input.focus(); input.setSelectionRange(focused.start, focused.end); }
    });
    refreshNoteIcons();
  }

  function setNoteCommentsOpen(open, remember = true) {
    const view = state.notesView;
    view.commentsOpen = open;
    const panel = $("[data-note-comments]");
    if (panel) panel.hidden = !open;
    renderNotePanelToggles();
    if (remember) rememberNoteSide();
    if (!open) { view.commentDraft = null; hideNoteMentions(); }
    renderNoteComments();
    renderNoteChat();
  }

  function noteFindRendered(markdown, text) {
    const exact = markdown.indexOf(text);
    if (exact !== -1) return { start: exact, end: exact + text.length };
    const lines = text.split(/\n+/).map((line) => line.trim()).filter(Boolean).sort((a, b) => b.length - a.length);
    for (const line of lines) {
      const at = markdown.indexOf(line);
      if (at !== -1) return { start: at, end: at + line.length };
    }
    return null;
  }

  function noteSelectionRange() {
    const view = state.notesView;
    const area = $("[data-note-markdown]"), markdown = area?.value || "";
    let range = null;
    const live = view.live;
    if (view.selectionSource === "write" && area && area.selectionEnd > area.selectionStart) {
      range = { start: area.selectionStart, end: area.selectionEnd };
    } else if (view.selectionSource === "live" && live && live.area.selectionEnd > live.area.selectionStart) {
      const base = noteLineOffset(markdown.split("\n"), live.start);
      range = { start: base + live.area.selectionStart, end: base + live.area.selectionEnd };
    } else {
      const selection = window.getSelection();
      const text = String(selection?.toString() || "").trim();
      if (text && $("[data-note-preview]")?.contains(selection.anchorNode)) range = noteFindRendered(markdown, text);
    }
    if (!range) return null;
    let { start, end } = range;
    while (start < end && /\s/.test(markdown[start])) start += 1;
    while (end > start && /\s/.test(markdown[end - 1])) end -= 1;
    return end > start ? { start, end } : null;
  }

  function noteSelectionAnchor() {
    const range = noteSelectionRange();
    if (!range) return null;
    const markdown = $("[data-note-markdown]")?.value || "";
    const { start } = range, end = Math.min(range.end, start + NOTE_QUOTE_MAX);
    return {
      quote: markdown.slice(start, end),
      prefix: markdown.slice(Math.max(0, start - NOTE_COMMENT_CONTEXT), start),
      suffix: markdown.slice(end, end + NOTE_COMMENT_CONTEXT),
      start, end,
    };
  }

  function startNoteComment(anchor) {
    const view = state.notesView;
    if (!view.selected) return;
    view.commentDraft = anchor || { quote: "", prefix: "", suffix: "", start: 0, end: 0 };
    $("[data-note-comment-float]").hidden = true;
    $("[data-note-react-float]").hidden = true;
    openNotePanel("comments");
    $("[data-note-comment-new] [data-note-comment-input]")?.focus();
  }

  function commentOnNoteSelection() {
    const anchor = noteSelectionAnchor();
    if (!anchor) { noteNotice("main", "Select some text in the note first, or use the comments panel to comment on the whole note.", "bad"); return; }
    noteNotice("main", "");
    startNoteComment(anchor);
  }

  function showNoteCommentFloat(event) {
    const view = state.notesView;
    const button = $("[data-note-comment-float]");
    if (!button || !view.selected) return;
    setTimeout(() => {
      const anchor = noteSelectionAnchor(), assist = $("[data-note-assist-float]"), react = $("[data-note-react-float]");
      if (!anchor) { button.hidden = true; if (assist) assist.hidden = true; if (react) react.hidden = true; return; }
      const body = $(".notes-body").getBoundingClientRect();
      const left = Math.max(8, Math.min(body.width - 220, event.clientX - body.left + 8));
      const top = Math.max(8, event.clientY - body.top + 12);
      button.style.left = `${left}px`;
      button.style.top = `${top}px`;
      button.hidden = false;
      let next = left + button.offsetWidth + 6;
      if (react) {
        react.hidden = false;
        react.style.left = `${next}px`;
        react.style.top = `${top}px`;
        next += react.offsetWidth + 6;
      }
      if (assist) {
        assist.hidden = !noteCanEdit(view.selected) || view.assistReady === false || Boolean(view.assist && !$("[data-note-assist-panel]").hidden);
        assist.style.left = `${next}px`;
        assist.style.top = `${top}px`;
      }
      refreshNoteIcons();
    }, 0);
  }

  async function loadNoteAssistStatus() {
    const view = state.notesView;
    if (view.assistStatus || !state.session) return;
    view.assistStatus = true;
    try {
      const payload = await noteRequest("/api/notes/assist");
      setNoteAssistReady(payload.ready !== false);
    } catch (_) { view.assistStatus = false; }
  }

  function setNoteAssistReady(ready) {
    state.notesView.assistReady = ready;
    const button = $("[data-note-assist-open]"); if (button) button.hidden = !ready;
    if (!ready) { const float = $("[data-note-assist-float]"); if (float) float.hidden = true; }
  }

  function noteAssistTarget() {
    const view = state.notesView, area = $("[data-note-markdown]");
    if (!area) return null;
    const range = noteSelectionRange();
    if (range) return { ...range, selected: true };
    let at = area.value.length;
    if (view.live && view.selectionSource === "live") {
      at = noteLineOffset(area.value.split("\n"), view.live.start) + view.live.area.selectionEnd;
    } else if (view.selectionSource === "write" || document.activeElement === area) {
      at = area.selectionEnd;
    }
    return { start: at, end: at, selected: false };
  }

  function openNoteAssist(reference) {
    const view = state.notesView, panel = $("[data-note-assist-panel]"), area = $("[data-note-markdown]");
    if (!panel || !area || !view.selected) return;
    if (!noteCanEdit(view.selected)) { noteNotice("main", NOTE_ERRORS.editor_required, "bad"); return; }
    const target = noteAssistTarget(); if (!target) return;
    const markdown = area.value, text = markdown.slice(target.start, target.end);
    if (text.length > NOTE_ASSIST_MAX) { noteNotice("main", NOTE_ERRORS.text_too_long, "bad"); return; }
    closeNotePopovers();
    view.assist = { noteId: view.selected.id, ...target, text, markdown, action: "", result: null, busy: false };
    $("[data-note-comment-float]").hidden = true;
    $("[data-note-assist-float]").hidden = true;
    $("[data-note-react-float]").hidden = true;
    $("[data-note-assist-prompt]").value = "";
    noteNotice("assist", "");
    panel.hidden = false;
    $("[data-note-assist-open]")?.setAttribute("aria-expanded", "true");
    renderNoteAssist();
    const body = $(".notes-body").getBoundingClientRect();
    const rect = (reference || $("[data-note-assist-open]") || area).getBoundingClientRect();
    const width = panel.offsetWidth;
    panel.style.left = `${Math.max(8, Math.min(body.width - width - 8, rect.left - body.left))}px`;
    panel.style.top = `${Math.max(8, rect.bottom - body.top + 6)}px`;
    $("[data-note-assist-prompt]").focus();
    void loadNoteAssistStatus();
  }

  function closeNoteAssist() {
    const view = state.notesView, panel = $("[data-note-assist-panel]");
    view.assist = null;
    if (panel) panel.hidden = true;
    $("[data-note-assist-open]")?.setAttribute("aria-expanded", "false");
  }

  function renderNoteAssist() {
    const assist = state.notesView.assist; if (!assist) return;
    const selected = assist.selected;
    $("[data-note-assist-heading]").textContent = selected ? "Rewrite with AI" : "Write with AI";
    const source = $("[data-note-assist-source]");
    source.hidden = !selected;
    source.textContent = selected ? assist.text.slice(0, 400) : "";
    const prompt = $("[data-note-assist-prompt]");
    prompt.placeholder = selected ? "Tell AI how to change the selection…" : "Tell AI what to write here…";
    prompt.disabled = assist.busy;
    const ask = $("[data-note-assist-ask]");
    ask.disabled = assist.busy;
    ask.textContent = assist.busy && assist.action === "custom" ? "Writing…" : "Ask";
    const actions = $("[data-note-assist-actions]");
    actions.hidden = !selected;
    actions.innerHTML = selected ? NOTE_ASSIST_ACTIONS.map(([id, label]) =>
      `<button type="button" class="notes-btn" data-note-assist-action="${id}" aria-pressed="${assist.action === id}"${assist.busy ? " disabled" : ""}>${escapeHtml(assist.busy && assist.action === id ? "Writing…" : label)}</button>`).join("") : "";
    const result = assist.result !== null;
    $("[data-note-assist-result-sec]").hidden = !result;
    if (result && $("[data-note-assist-result]").dataset.for !== String(assist.resultId)) {
      $("[data-note-assist-result]").value = assist.result;
      $("[data-note-assist-result]").dataset.for = String(assist.resultId);
    }
    const replace = $("[data-note-assist-apply=\"replace\"]");
    replace.lastChild.textContent = selected ? "Replace selection" : "Insert";
    $("[data-note-assist-apply=\"below\"]").hidden = !selected;
    $("[data-note-assist-panel]").querySelectorAll("[data-note-assist-apply], [data-note-assist-retry]").forEach((button) => { button.disabled = assist.busy; });
    refreshNoteIcons();
  }

  async function runNoteAssist(action) {
    const view = state.notesView, assist = view.assist;
    if (!assist || assist.busy) return;
    const prompt = action === "custom" ? $("[data-note-assist-prompt]").value.trim() : "";
    if (action === "custom" && !prompt) {
      noteNotice("assist", assist.selected ? "Type how to change the selection, or pick a quick action." : "Type what you want the AI to write.", "bad");
      $("[data-note-assist-prompt]").focus();
      return;
    }
    Object.assign(assist, { action, prompt, busy: true });
    noteNotice("assist", "");
    renderNoteAssist();
    try {
      const payload = await noteRequest("/api/notes/assist", "POST", {
        action, prompt, text: assist.text,
        before: assist.markdown.slice(Math.max(0, assist.start - NOTE_ASSIST_CONTEXT), assist.start),
        after: assist.markdown.slice(assist.end, assist.end + NOTE_ASSIST_CONTEXT),
      });
      if (view.assist !== assist) return;
      assist.result = String(payload.text || "");
      assist.resultId = (assist.resultId || 0) + 1;
    } catch (error) {
      if (view.assist !== assist) return;
      if (error.payload?.error === "assist_unavailable") setNoteAssistReady(false);
      noteNotice("assist", error.message, "bad");
    } finally {
      assist.busy = false;
      if (view.assist === assist) renderNoteAssist();
    }
    if (view.assist === assist && assist.result !== null) $("[data-note-assist-result]").focus();
  }

  function applyNoteAssist(mode) {
    const view = state.notesView, assist = view.assist, area = $("[data-note-markdown]");
    if (!assist || !area || view.selected?.id !== assist.noteId || !noteCanEdit(view.selected)) return;
    const text = $("[data-note-assist-result]").value.trim();
    if (!text) return;
    if (view.live) endNoteLiveEdit(false);
    const value = area.value;
    let { start, end } = assist;
    if (value.slice(start, end) !== assist.text) {
      const found = assist.text ? value.indexOf(assist.text) : Math.min(start, value.length);
      if (found === -1) { noteNotice("assist", "The note changed and the selected text is gone. Copy the suggestion, or close this and try again.", "bad"); return; }
      start = found; end = found + assist.text.length;
    }
    let at = start, cut = end, insert = text;
    if (mode === "below") {
      const lineEnd = value.indexOf("\n", end);
      at = cut = lineEnd === -1 ? value.length : lineEnd;
      insert = `\n\n${text}`;
    }
    if (!assist.selected) {
      const lineStart = at === 0 || value[at - 1] === "\n";
      if (!lineStart && !/\s/.test(value[at - 1])) insert = ` ${insert}`;
      if (at < value.length && !/\s/.test(value[at])) insert += lineStart ? "\n\n" : " ";
    }
    area.value = value.slice(0, at) + insert + value.slice(cut);
    closeNoteAssist();
    if (area.offsetParent) {
      area.focus();
      const from = at + insert.indexOf(text);
      area.setSelectionRange(from, from + text.length);
    }
    area.dispatchEvent(new Event("input", { bubbles: true }));
    noteNotice("main", mode === "below" ? "Added the AI text below the selection." : assist.selected ? "Replaced the selection with the AI text." : "Added the AI text.");
  }

  function wireNoteAssist() {
    const view = state.notesView, panel = $("[data-note-assist-panel]");
    if (!panel) return;
    for (const trigger of ["[data-note-assist-open]", "[data-note-assist-float]"]) {
      $(trigger)?.addEventListener("mousedown", (event) => event.preventDefault());
      $(trigger)?.addEventListener("click", (event) => {
        event.stopPropagation();
        if (!panel.hidden && trigger === "[data-note-assist-open]") { closeNoteAssist(); return; }
        openNoteAssist(event.currentTarget);
      });
    }
    $("[data-note-assist-form]")?.addEventListener("submit", (event) => { event.preventDefault(); void runNoteAssist("custom"); });
    panel.addEventListener("click", (event) => {
      const action = event.target.closest("[data-note-assist-action]");
      if (action) { void runNoteAssist(action.dataset.noteAssistAction); return; }
      const apply = event.target.closest("[data-note-assist-apply]");
      if (apply) { applyNoteAssist(apply.dataset.noteAssistApply); return; }
      if (event.target.closest("[data-note-assist-retry]") && view.assist?.action) { void runNoteAssist(view.assist.action); return; }
      if (event.target.closest("[data-note-assist-discard]")) closeNoteAssist();
    });
    document.addEventListener("click", (event) => {
      if (panel.hidden || event.composedPath().some((node) => node === panel || node?.matches?.("[data-note-assist-open], [data-note-assist-float]"))) return;
      closeNoteAssist();
    });
  }

  function jumpToNoteThread(id) {
    const view = state.notesView;
    view.commentFocus = id;
    const thread = noteCommentThreads().open.concat(noteCommentThreads().resolved).find((item) => item.id === id);
    renderNoteComments();
    renderNotePreview();
    if (!thread?.anchor) return;
    const area = $("[data-note-markdown]");
    if (area && !["preview", "live"].includes($("[data-note-work]").dataset.mode)) {
      area.focus({ preventScroll: true });
      area.setSelectionRange(thread.anchor.start, thread.anchor.end);
      const lines = area.value.slice(0, thread.anchor.start).split("\n").length;
      const lineHeight = parseFloat(getComputedStyle(area).lineHeight) || 20;
      area.scrollTop = Math.max(0, (lines - 3) * lineHeight);
      area.closest(".notes-pane")?.scrollTo?.({ top: Math.max(0, area.offsetTop + (lines - 3) * lineHeight - 40), behavior: "smooth" });
    }
    $(`[data-note-thread-mark="${id}"]`)?.scrollIntoView({ block: "center", behavior: "smooth" });
  }

  function markNoteCommentsInPreview(preview) {
    const view = state.notesView;
    if (!view.comments || !preview) return;
    for (const thread of noteCommentThreads().open) {
      if (!thread.anchor) continue;
      const needle = thread.quote.split("\n").map((line) => line.replace(/[*_`#>~]|\[|\]\([^)]*\)|^\s*[-+]\s+(\[[ xX]\]\s+)?|^\s*\d+\.\s+/g, "").trim()).find(Boolean);
      if (!needle) continue;
      const walker = document.createTreeWalker(preview, NodeFilter.SHOW_TEXT);
      for (let node = walker.nextNode(); node; node = walker.nextNode()) {
        if (node.parentElement?.closest(".notes-comment-mark")) continue;
        const at = node.nodeValue.indexOf(needle);
        if (at === -1) continue;
        const target = node.splitText(at);
        target.splitText(needle.length);
        const mark = document.createElement("mark");
        mark.className = "notes-comment-mark";
        mark.dataset.noteThreadMark = thread.id;
        if (view.commentFocus === thread.id) mark.dataset.focus = "true";
        mark.title = `${thread.author || "Someone"}: ${thread.body}`.slice(0, 200);
        target.replaceWith(mark);
        mark.appendChild(target);
        break;
      }
    }
  }

  function noteReactionGroups(markdown) {
    const groups = new Map();
    for (const reaction of state.notesView.comments?.reactions || []) {
      let key = "note", range = null;
      if (reaction.quote) {
        range = noteLocateQuote(markdown, reaction);
        if (!range) continue;
        key = `${range.start}:${range.end}`;
      }
      if (!groups.has(key)) groups.set(key, { key, range, anchor: reaction, emojis: new Map() });
      const emojis = groups.get(key).emojis;
      if (!emojis.has(reaction.emoji)) emojis.set(reaction.emoji, []);
      emojis.get(reaction.emoji).push(reaction);
    }
    return groups;
  }

  function noteReactionRowHtml(group) {
    const me = noteMe();
    const quote = String(group.anchor.quote || "").replace(/\s+/g, " ").trim();
    const label = quote ? `<span class="notes-reaction-quote" title="${escapeHtml(quote)}">“${escapeHtml(quote.length > 40 ? `${quote.slice(0, 39)}…` : quote)}”</span>` : "";
    const chips = [...group.emojis].map(([emoji, reactions]) => {
      const names = reactions.map((reaction) => reaction.author || "Someone").join(", ");
      const mine = reactions.some((reaction) => reaction.author && reaction.author === me);
      return `<button type="button" class="notes-reaction" data-note-reaction-toggle="${escapeHtml(group.key)}" data-emoji="${escapeHtml(emoji)}" aria-pressed="${mine}" title="${escapeHtml(`${names} reacted with ${emoji}`)}"><span>${escapeHtml(emoji)}</span><span class="notes-reaction-count">${reactions.length}</span></button>`;
    }).join("");
    return `${label}${chips}<button type="button" class="notes-reaction notes-reaction-add" data-note-reaction-add="${escapeHtml(group.key)}" title="Add a reaction" aria-label="Add a reaction">${noteIcon("smile-plus")}</button>`;
  }

  function renderNoteReactions(preview, markdown) {
    const view = state.notesView, live = noteLiveMode();
    view.reactionGroups = noteReactionGroups(markdown);
    const whole = view.reactionGroups.get("note"), top = $("[data-note-reactions-note]");
    if (top) { top.innerHTML = whole ? noteReactionRowHtml(whole) : ""; top.hidden = !whole; }
    const lines = markdown.split("\n");
    const blocks = live ? [...preview.children].filter((child) => child.matches(".notes-live-block")) : [];
    const last = new Map();
    for (const group of [...view.reactionGroups.values()].filter((item) => item.range).sort((a, b) => a.range.start - b.range.start)) {
      const line = markdown.slice(0, group.range.start).split("\n").length - 1;
      let target = null;
      if (live) {
        for (const block of blocks) if (Number(block.dataset.liveLine) <= line) target = block;
        if (target?.dataset.editing === "true") continue;
      } else target = noteFollowPreviewElement(preview, lines, line);
      if (!target) continue;
      const row = document.createElement("div");
      row.className = "notes-reactions";
      row.innerHTML = noteReactionRowHtml(group);
      if (last.has(target)) last.get(target).after(row);
      else if (live || target.matches("li, td, th")) target.appendChild(row);
      else target.after(row);
      last.set(target, row);
    }
    refreshNoteIcons();
  }

  function renderNotePeerMarks() {
    const view = state.notesView, preview = $("[data-note-preview]");
    document.querySelectorAll("[data-view=\"notes\"] [data-peer-editing]").forEach((node) => {
      node.removeAttribute("data-peer-editing");
      node.style.removeProperty("--peer-color");
    });
    if (!preview || !view.selected || $("[data-note-work]")?.dataset.mode === "write") return;
    const lines = ($("[data-note-markdown]")?.value || "").split("\n"), live = noteLiveMode();
    const blocks = live ? [...preview.children].filter((child) => child.matches(".notes-live-block")) : [];
    const marks = new Map();
    const now = Date.now();
    for (const peer of view.peers?.values() || []) {
      const editing = peer.state === "editing" || now - (peer.draftAt || 0) < 5000;
      if (!editing || !(peer.line > 0) || now - peer.seen > NOTE_PRESENCE_MS * 2.5) continue;
      const index = Math.min(lines.length - 1, peer.line - 1);
      let target = null;
      if (live) { for (const block of blocks) if (Number(block.dataset.liveLine) <= index) target = block; }
      else target = noteFollowPreviewElement(preview, lines, index);
      if (!target) continue;
      if (!marks.has(target)) marks.set(target, []);
      if (!marks.get(target).includes(peer.name)) marks.get(target).push(peer.name);
    }
    for (const [target, names] of marks) {
      target.dataset.peerEditing = names.join(", ");
      target.style.setProperty("--peer-color", noteAvatarColor(names[0]));
    }
  }

  function noteReactionAnchor(source) {
    return {
      quote: String(source?.quote || ""), prefix: String(source?.prefix || ""), suffix: String(source?.suffix || ""),
      start: Number(source?.start) || 0, end: Number(source?.end) || 0,
    };
  }

  async function sendNoteReaction(path, method, body) {
    const view = state.notesView, note = view.selected;
    if (!note) return;
    try {
      const payload = await noteRequest(`/api/notes/${note.id}/reactions${path}`, method, body);
      view.commentsGeneration++;
      applyNoteComments(note.id, payload);
    } catch (error) {
      noteNotice("main", error.message, "bad");
    }
  }

  function toggleNoteReaction(key, emoji) {
    const group = state.notesView.reactionGroups.get(key);
    if (!group) return;
    const mine = (group.emojis.get(emoji) || []).find((reaction) => reaction.author && reaction.author === noteMe());
    if (mine) void sendNoteReaction(`/${mine.id}`, "DELETE");
    else void sendNoteReaction("", "POST", { emoji, ...noteReactionAnchor(group.anchor) });
  }

  function closeNoteReactionPicker() {
    const picker = $("[data-note-reaction-picker]");
    if (picker) picker.hidden = true;
    state.notesView.reactionAnchor = null;
  }

  function openNoteReactionPicker(anchor, reference) {
    const view = state.notesView, picker = $("[data-note-reaction-picker]"), body = $(".notes-body");
    if (!picker || !body || !view.selected) return;
    view.reactionAnchor = noteReactionAnchor(anchor);
    picker.innerHTML = NOTE_REACTIONS.map((emoji) =>
      `<button type="button" data-note-reaction-pick="${escapeHtml(emoji)}" role="menuitem" aria-label="React with ${escapeHtml(emoji)}" title="React with ${escapeHtml(emoji)}">${escapeHtml(emoji)}</button>`).join("");
    picker.hidden = false;
    const box = body.getBoundingClientRect(), rect = reference.getBoundingClientRect();
    picker.style.left = `${Math.max(8, Math.min(box.width - picker.offsetWidth - 8, rect.left - box.left))}px`;
    picker.style.top = `${Math.max(8, rect.bottom - box.top + 6)}px`;
    picker.querySelector("button")?.focus({ preventScroll: true });
  }

  function reactToNoteSelection(event) {
    const anchor = noteSelectionAnchor();
    if (!anchor) { noteNotice("main", "Select some text in the note first.", "bad"); return; }
    $("[data-note-comment-float]").hidden = true;
    $("[data-note-assist-float]").hidden = true;
    openNoteReactionPicker(anchor, event.currentTarget);
    $("[data-note-react-float]").hidden = true;
  }

  function wireNoteReactions() {
    const float = $("[data-note-react-float]");
    float?.addEventListener("mousedown", (event) => event.preventDefault());
    float?.addEventListener("click", reactToNoteSelection);
    $("[data-note-preview-pane]")?.addEventListener("click", (event) => {
      const toggle = event.target.closest("[data-note-reaction-toggle]");
      if (toggle) { toggleNoteReaction(toggle.dataset.noteReactionToggle, toggle.dataset.emoji); return; }
      const add = event.target.closest("[data-note-reaction-add]");
      const group = add && state.notesView.reactionGroups.get(add.dataset.noteReactionAdd);
      if (group) openNoteReactionPicker(group.anchor, add);
    });
    const picker = $("[data-note-reaction-picker]");
    picker?.addEventListener("mousedown", (event) => event.preventDefault());
    picker?.addEventListener("click", (event) => {
      const pick = event.target.closest("[data-note-reaction-pick]");
      const anchor = state.notesView.reactionAnchor;
      if (!pick || !anchor) return;
      closeNoteReactionPicker();
      void sendNoteReaction("", "POST", { emoji: pick.dataset.noteReactionPick, ...anchor });
    });
    document.addEventListener("click", (event) => {
      if (!picker || picker.hidden || event.composedPath().some((node) => node === picker || node?.matches?.("[data-note-react-float], [data-note-reaction-add]"))) return;
      closeNoteReactionPicker();
    });
  }

  function hideNoteMentions() {
    const list = $("[data-note-mention-list]");
    if (list) list.hidden = true;
    state.notesView.mention = null;
  }

  function renderNoteMentions(input) {
    const view = state.notesView;
    const list = $("[data-note-mention-list]");
    const before = input.value.slice(0, input.selectionStart);
    const match = /(^|[^\w@])@([a-z0-9-]{0,62})$/i.exec(before);
    const me = noteMe();
    const options = match ? (view.comments?.people || []).filter((name) => name !== me && name.startsWith(match[2].toLowerCase())).slice(0, 6) : [];
    if (!options.length) { hideNoteMentions(); return; }
    const index = view.mention?.input === input ? Math.min(view.mention.index, options.length - 1) : 0;
    view.mention = { input, options, index, from: before.length - match[2].length - 1 };
    input.closest("form").appendChild(list);
    list.innerHTML = options.map((name, position) => `<button type="button" role="option" data-note-mention-option="${position}" aria-selected="${position === index ? "true" : "false"}">
        <span class="notes-avatar" style="background:${noteAvatarColor(name)}">${escapeHtml(noteInitials(name))}</span>
        <span class="notes-person-name">${escapeHtml(name)}</span><span></span>
      </button>`).join("");
    list.hidden = false;
  }

  function chooseNoteMention(position) {
    const mention = state.notesView.mention;
    if (!mention) return;
    const input = mention.input, name = mention.options[position];
    const caret = input.selectionStart;
    input.value = `${input.value.slice(0, mention.from)}@${name} ${input.value.slice(caret)}`;
    const next = mention.from + name.length + 2;
    input.focus(); input.setSelectionRange(next, next);
    hideNoteMentions();
  }

  async function submitNoteCommentForm(form) {
    const view = state.notesView;
    const input = form.querySelector("[data-note-comment-input]");
    const body = input.value.trim();
    if (!body) { input.focus(); return; }
    if (body.length > 4000) { noteNotice("comments", "Comments can be up to 4000 characters.", "bad"); return; }
    hideNoteMentions();
    form.querySelectorAll("button, textarea").forEach((control) => { control.disabled = true; });
    let done;
    if (form.matches("[data-note-comment-new]")) {
      const draft = view.commentDraft || {};
      done = await sendNoteComment("", "POST", draft.quote ? { body, quote: draft.quote, prefix: draft.prefix, suffix: draft.suffix, start: draft.start, end: draft.end } : { body });
      if (done) { view.commentDraft = null; view.commentFocus = done.comment?.id || ""; input.value = ""; }
    } else if (form.dataset.noteReplyForm) {
      done = await sendNoteComment("", "POST", { body, parentId: form.dataset.noteReplyForm });
      if (done) input.value = "";
    } else if (form.dataset.noteCommentEditForm) {
      done = await sendNoteComment(`/${form.dataset.noteCommentEditForm}`, "PATCH", { body });
      if (done) view.commentEditing = "";
    }
    form.querySelectorAll("button, textarea").forEach((control) => { control.disabled = false; });
    renderNoteComments();
  }

  function wireNoteComments() {
    const view = state.notesView;
    $("[data-note-comments-toggle]")?.addEventListener("click", () => toggleNotePanel("comments"));
    $("[data-note-comments-close]")?.addEventListener("click", () => setNoteCommentsOpen(false));
    $("[data-note-comment-general]")?.addEventListener("click", () => startNoteComment(null));
    $("[data-note-comment-selection]")?.addEventListener("click", commentOnNoteSelection);
    $("[data-note-comment-float]")?.addEventListener("mousedown", (event) => event.preventDefault());
    $("[data-note-comment-float]")?.addEventListener("click", commentOnNoteSelection);
    const area = $("[data-note-markdown]");
    for (const type of ["mouseup", "keyup", "select"]) area?.addEventListener(type, () => { view.selectionSource = "write"; });
    area?.addEventListener("mouseup", showNoteCommentFloat);
    $("[data-note-preview]")?.addEventListener("mouseup", (event) => {
      view.selectionSource = event.target.matches?.("[data-note-live-input]") ? "live" : "preview";
      if (view.selectionSource === "live") sendNoteCursor();
      showNoteCommentFloat(event);
    });
    $("[data-note-preview]")?.addEventListener("click", (event) => {
      const mark = event.target.closest("[data-note-thread-mark]");
      if (!mark || String(window.getSelection()?.toString() || "")) return;
      openNotePanel("comments"); jumpToNoteThread(mark.dataset.noteThreadMark);
      $(`[data-note-thread="${mark.dataset.noteThreadMark}"]`)?.scrollIntoView({ block: "nearest" });
    });
    document.addEventListener("selectionchange", () => {
      const button = $("[data-note-comment-float]"), assist = $("[data-note-assist-float]"), react = $("[data-note-react-float]");
      if (button && !button.hidden && !noteSelectionAnchor()) { button.hidden = true; if (assist) assist.hidden = true; if (react) react.hidden = true; }
    });
    const panel = $("[data-note-comments]");
    panel?.addEventListener("submit", (event) => {
      const form = event.target.closest("form"); if (!form) return;
      event.preventDefault(); void submitNoteCommentForm(form);
    });
    panel?.addEventListener("input", (event) => {
      if (event.target.matches("[data-note-comment-input]")) renderNoteMentions(event.target);
    });
    panel?.addEventListener("keydown", (event) => {
      if (!event.target.matches("[data-note-comment-input]")) return;
      const mention = view.mention;
      if (mention && !$("[data-note-mention-list]").hidden) {
        if (event.key === "ArrowDown" || event.key === "ArrowUp") {
          event.preventDefault();
          mention.index = (mention.index + (event.key === "ArrowDown" ? 1 : -1) + mention.options.length) % mention.options.length;
          renderNoteMentions(event.target); return;
        }
        if (event.key === "Enter" || event.key === "Tab") { event.preventDefault(); chooseNoteMention(mention.index); return; }
        if (event.key === "Escape") { event.stopPropagation(); hideNoteMentions(); return; }
      }
      if (event.key === "Enter" && !event.shiftKey && !event.isComposing) {
        event.preventDefault(); void submitNoteCommentForm(event.target.closest("form"));
      }
    });
    panel?.addEventListener("focusout", (event) => {
      if (event.target.matches("[data-note-comment-input]")) setTimeout(() => { if (!panel.contains(document.activeElement) || !document.activeElement.matches("[data-note-comment-input]")) hideNoteMentions(); }, 150);
    });
    $("[data-note-mention-list]")?.addEventListener("mousedown", (event) => event.preventDefault());
    $("[data-note-mention-list]")?.addEventListener("click", (event) => {
      const option = event.target.closest("[data-note-mention-option]");
      if (option) chooseNoteMention(Number(option.dataset.noteMentionOption));
    });
    panel?.addEventListener("click", (event) => {
      const jump = event.target.closest("[data-note-thread-jump]");
      if (jump) { jumpToNoteThread(jump.dataset.noteThreadJump); return; }
      const resolve = event.target.closest("[data-note-thread-resolve]");
      if (resolve) {
        const thread = (view.comments?.comments || []).find((item) => item.id === resolve.dataset.noteThreadResolve);
        if (thread) void sendNoteComment(`/${thread.id}`, "PATCH", { resolved: !thread.resolvedAt }, thread.resolvedAt ? "Thread reopened" : "Thread resolved");
        return;
      }
      const edit = event.target.closest("[data-note-comment-edit]");
      if (edit) { view.commentEditing = edit.dataset.noteCommentEdit; renderNoteComments(); panel.querySelector(`[data-note-comment-edit-form="${view.commentEditing}"] textarea`)?.focus(); return; }
      if (event.target.closest("[data-note-comment-edit-cancel]")) { view.commentEditing = ""; renderNoteComments(); return; }
      if (event.target.closest("[data-note-comment-cancel]")) { view.commentDraft = null; renderNoteComments(); return; }
      const remove = event.target.closest("[data-note-comment-delete]");
      if (remove) {
        const comment = (view.comments?.comments || []).find((item) => item.id === remove.dataset.noteCommentDelete);
        if (!comment) return;
        const replies = (view.comments.comments || []).filter((item) => item.parentId === comment.id).length;
        if (!window.confirm(replies ? `Delete this comment and its ${noteCountLabel(replies, "reply", "replies")}?` : "Delete this comment?")) return;
        void sendNoteComment(`/${comment.id}`, "DELETE", {}, "Comment deleted");
        return;
      }
      const thread = event.target.closest("[data-note-thread]");
      if (thread && !event.target.closest("button, a, textarea, form") && view.commentFocus !== thread.dataset.noteThread) jumpToNoteThread(thread.dataset.noteThread);
    });
    $("[data-note-presence]")?.addEventListener("click", (event) => {
      event.stopPropagation(); toggleNotePopover("[data-note-people-panel]", "[data-note-presence]"); renderNotePresence();
    });
    $("[data-note-live]")?.addEventListener("click", (event) => {
      event.stopPropagation(); toggleNotePopover("[data-note-people-panel]", "[data-note-presence]"); renderNotePresence();
    });
  }

  function openNoteEmojiPicker(target, trigger) {
    const view = state.notesView, panel = $("[data-note-emoji-panel]");
    if (!panel || !trigger) return;
    const wasOpen = !panel.hidden && view.emojiTarget === target;
    closeNotePopovers();
    if (wasOpen) return;
    view.emojiTarget = target;
    const current = (target === "page" ? view.pageDraft?.emoji : view.selected?.emoji) || "";
    $("[data-note-emoji-grid]").innerHTML = NOTE_EMOJI_CHOICES.map((emoji) =>
      `<button type="button" data-note-emoji-choice="${emoji}" aria-label="${emoji}" aria-pressed="${emoji === current ? "true" : "false"}">${emoji}</button>`).join("");
    const input = $("[data-note-emoji-input]");
    input.value = "";
    $("[data-note-emoji-remove]").disabled = !current;
    noteNotice("emoji", "");
    panel.hidden = false;
    const app = $("[data-note-app]").getBoundingClientRect(), at = trigger.getBoundingClientRect();
    panel.style.top = `${Math.max(8, at.bottom - app.top + 6)}px`;
    panel.style.left = `${Math.max(8, Math.min(app.width - panel.offsetWidth - 8, at.left - app.left))}px`;
    trigger.setAttribute("aria-expanded", "true");
    input.focus();
    refreshNoteIcons();
  }

  function chooseNoteEmoji(emoji) {
    const view = state.notesView;
    if (emoji && !noteEmojiValid(emoji)) { noteNotice("emoji", NOTE_ERRORS.invalid_emoji, "bad"); return; }
    closeNotePopovers();
    if (view.emojiTarget === "page") {
      if (!view.pageDraft) return;
      view.pageDraft.emoji = emoji;
      renderNotePageDraftEmoji();
      $("[data-note-page-emoji]")?.focus();
      return;
    }
    const note = view.selected;
    if (!note || !noteCanEdit(note)) return;
    $("[data-note-emoji-open]")?.focus();
    if ((note.emoji || "") === emoji && view.emojiPending === null) return;
    view.emojiPending = emoji;
    storeNote({ ...note, emoji });
    renderNoteEditor(view.selected, { keepDraft: true });
    void saveNote();
  }

  function wireNoteEmoji() {
    $("[data-note-emoji-open]")?.addEventListener("click", (event) => {
      event.stopPropagation(); openNoteEmojiPicker("note", event.currentTarget);
    });
    const panel = $("[data-note-emoji-panel]");
    panel?.addEventListener("click", (event) => {
      const choice = event.target.closest("[data-note-emoji-choice]");
      if (choice) { chooseNoteEmoji(choice.dataset.noteEmojiChoice); return; }
      if (event.target.closest("[data-note-emoji-remove]")) chooseNoteEmoji("");
    });
    $("[data-note-emoji-form]")?.addEventListener("submit", (event) => {
      event.preventDefault();
      const value = $("[data-note-emoji-input]").value.trim();
      if (!value) { $("[data-note-emoji-input]").focus(); return; }
      chooseNoteEmoji(value);
    });
    panel?.addEventListener("keydown", (event) => {
      if (event.key !== "Escape") return;
      event.stopPropagation(); closeNotePopovers();
      (state.notesView.emojiTarget === "page" ? $("[data-note-page-emoji]") : $("[data-note-emoji-open]"))?.focus();
    });
  }

  function notePageAccent(accent) { return NOTE_PAGE_ACCENTS[accent] || NOTE_PAGE_ACCENTS.blue; }

  function notePageCanEdit(page) { return ["owner", "editor"].includes(page?.role); }

  function notePageUrl(id) { return `${location.origin}/dashboard/notes?page=${encodeURIComponent(id)}`; }

  function notePageEntryId() { return crypto.randomUUID().replace(/-/g, "").slice(0, 16); }

  function notePageEntryTitle(entry, note) {
    const label = String(entry.label || "").trim();
    if (label) return label;
    if (entry.kind === "note") return note?.title || "Untitled note";
    if (entry.kind === "link") return String(entry.url || "").replace(/^https:\/\//, "");
    return "Untitled section";
  }

  function notePageRows(outline, collapsed) {
    const open = [];
    return outline.map((entry) => {
      while (open.length && open[open.length - 1].depth >= entry.depth) open.pop();
      const hidden = open.some((section) => collapsed.get(section.id));
      const parents = open.map((section) => section.id);
      if (entry.kind === "section") open.push(entry);
      return { entry, hidden, parents };
    });
  }

  function notePageSummary(page) {
    return {
      id: page.id, title: page.title, emoji: page.emoji, description: page.description, accent: page.accent,
      layout: page.layout, showEmoji: page.showEmoji, role: page.role, version: page.version,
      createdAt: page.createdAt, updatedAt: page.updatedAt, shares: page.shares || [],
      entries: Array.isArray(page.outline) ? page.outline.length : Number(page.entries || 0),
    };
  }

  function renderNotePages() {
    const box = $("[data-note-pages]"); if (!box) return;
    const view = state.notesView, query = noteSearchQuery(), all = view.pages || [];
    const pages = all.filter((page) => !query || String(page.title || "").toLowerCase().includes(query));
    const rows = pages.map((page) => {
      const title = page.title || "Untitled page";
      const count = noteCountLabel(page.entries, "entry", "entries");
      return `<button type="button" class="notes-page-row" data-note-page-id="${escapeHtml(page.id)}" aria-selected="${view.pageId === page.id ? "true" : "false"}" title="${escapeHtml(`${title} · ${count}`)}">
          <span class="notes-page-row-icon">${page.emoji ? escapeHtml(page.emoji) : noteIcon("book-open")}</span>
          <span class="notes-row-title">${escapeHtml(title)}</span>
          <span class="notes-page-mark" style="background:${notePageAccent(page.accent)}"></span>
          <span class="notes-page-count" aria-label="${escapeHtml(count)}">${Number(page.entries || 0)}</span>
        </button>`;
    }).join("");
    const empty = view.pagesError ? escapeHtml(view.pagesError)
      : view.pages === null ? "Loading pages…"
      : query && all.length ? ""
      : all.length ? "" : "Gather notes into one page you can share.";
    box.innerHTML = `<div class="notes-group notes-pages-head"><span>Pages</span><button type="button" data-note-page-new class="notes-icon-btn" title="New page" aria-label="New page">${noteIcon("plus")}</button></div>`
      + (rows || (empty ? `<p class="notes-pages-empty">${empty}</p>` : ""));
    refreshNoteIcons();
  }

  async function loadNotePages() {
    const view = state.notesView;
    if (!state.session?.sessionToken) return;
    try {
      const payload = await noteRequest("/api/notes/pages");
      view.pages = (payload.pages || []).map(notePageSummary); view.pagesError = "";
    } catch (error) {
      if (view.pages === null) view.pages = [];
      view.pagesError = error.status === 404 ? "" : `Pages are unavailable: ${error.message}`;
    }
    renderNotePages();
    renderNoteList();
  }

  function renderNoteMain() {
    const view = state.notesView, paging = Boolean(view.pageId);
    $("[data-note-page-view]").hidden = !paging;
    $("[data-note-editor]").hidden = paging || !view.selected;
    $("[data-note-empty]").hidden = paging || Boolean(view.selected);
  }

  function leaveNoteEditor() {
    const view = state.notesView;
    closeNotePopovers();
    const socket = view.socket;
    view.socket = null; view.socketNoteId = ""; socket?.close();
    clearInterval(view.presenceTimer); view.peers = new Map();
    clearTimeout(view.saveTimer);
    Object.assign(view, {
      selected: null, localDirty: false, conflict: null, emojiPending: null, comments: null,
      commentDraft: null, commentEditing: "", commentFocus: "", activity: null, linking: false, versionShown: null,
    });
    $("[data-note-conflict]").hidden = true;
    renderNoteVersion();
  }

  async function openNotePage(id, { noteId = "", customize = false } = {}) {
    const view = state.notesView;
    await flushNoteSave();
    if (view.selected) leaveNoteEditor();
    closeNotePopovers();
    if (view.pageId !== id) {
      closeNotePageCustomize();
      view.page = null; view.pageContent = new Map(); view.pageCollapsed = new Map();
    }
    view.pageId = id; view.pageNoteId = noteId; view.pageOutlineOpen = false;
    rememberNotePageInUrl(id, noteId);
    renderNoteMain(); renderNotePage(); renderNotePages(); renderNoteList();
    showNoteEditorOnMobile();
    await loadNotePage(id);
    if (customize && view.pageId === id && view.page) openNotePageCustomize();
  }

  function closeNotePage() {
    const view = state.notesView;
    if (!view.pageId) return;
    closeNotePopovers(); closeNotePageCustomize();
    view.pageId = ""; view.page = null; view.pageNoteId = ""; view.pageContent = new Map();
    view.pageGeneration++;
    renderNoteMain(); renderNotePages();
  }

  async function loadNotePage(id) {
    const view = state.notesView, generation = ++view.pageGeneration;
    try {
      const payload = await noteRequest(`/api/notes/pages/${encodeURIComponent(id)}?content=1`);
      if (generation !== view.pageGeneration || view.pageId !== id) return;
      applyNotePage(payload);
    } catch (error) {
      if (generation !== view.pageGeneration || view.pageId !== id) return;
      if (error.status === 404) {
        view.pages = (view.pages || []).filter((page) => page.id !== id);
        closeNotePage();
        noteNotice("main", NOTE_ERRORS.page_not_found, "bad");
        await loadNotes();
      } else noteNotice("page", error.message, "bad");
    }
  }

  function applyNotePage(payload) {
    const view = state.notesView, page = payload.page || {}, notes = payload.notes || {};
    const outline = page.outline || [];
    view.page = { page: { ...page, outline }, notes };
    for (const [noteId, note] of Object.entries(notes)) {
      if (typeof note?.markdown === "string") view.pageContent.set(noteId, note.markdown);
    }
    for (const entry of outline) {
      if (entry.kind === "section" && !view.pageCollapsed.has(entry.id)) view.pageCollapsed.set(entry.id, Boolean(entry.collapsed));
    }
    const summary = notePageSummary(view.page.page);
    view.pages = (view.pages || []).some((row) => row.id === page.id)
      ? view.pages.map((row) => row.id === page.id ? summary : row)
      : [summary, ...(view.pages || [])];
    const readable = outline.filter((entry) => entry.kind === "note" && notes[entry.noteId]);
    if (!readable.some((entry) => entry.noteId === view.pageNoteId)) view.pageNoteId = readable[0]?.noteId || "";
    const picked = notePageRows(outline, view.pageCollapsed).find(({ entry }) => entry.kind === "note" && entry.noteId === view.pageNoteId);
    if (picked?.hidden && page.layout !== "continuous") picked.parents.forEach((sectionId) => view.pageCollapsed.set(sectionId, false));
    rememberNotePageInUrl(page.id, view.pageNoteId);
    renderNotePage(); renderNotePages();
    if (page.layout !== "continuous" && view.pageNoteId) markNotePageSeen(view.pageNoteId);
    void fillNotePageContent();
  }

  function markNotePageSeen(noteId) {
    const view = state.notesView, note = view.page?.notes?.[noteId];
    if (!note) return;
    if (note.unread) {
      note.unread = false;
      view.items = view.items.map((item) => item.id === noteId ? { ...item, unread: false, unreadComments: 0 } : item);
      renderNoteList(); renderNotePageOutline();
    }
    if (["owner", "editor", "viewer"].includes(note.role)) {
      void noteRequest(`/api/notes/${encodeURIComponent(noteId)}/activity`, "POST", { kind: "view" }).catch(() => {});
    }
  }

  async function fillNotePageContent() {
    const view = state.notesView, data = view.page;
    if (!data) return;
    const id = data.page.id, notes = data.notes || {};
    const wanted = data.page.layout === "continuous"
      ? [...new Set(data.page.outline.filter((entry) => entry.kind === "note" && notes[entry.noteId]).map((entry) => entry.noteId))]
      : [view.pageNoteId].filter((noteId) => noteId && notes[noteId]);
    const missing = wanted.filter((noteId) => !view.pageContent.has(noteId) && !view.pageFetching.has(noteId));
    if (!missing.length) return;
    missing.forEach((noteId) => view.pageFetching.add(noteId));
    for (let index = 0; index < missing.length; index += 4) {
      const content = view.pageContent;
      await Promise.all(missing.slice(index, index + 4).map(async (noteId) => {
        try {
          const payload = await noteRequest(`/api/notes/${encodeURIComponent(noteId)}`);
          content.set(noteId, String(payload.note?.markdown || ""));
        } catch (_) {
          content.set(noteId, null);
        } finally { view.pageFetching.delete(noteId); }
      }));
      if (view.pageId === id) renderNotePageNotes();
    }
  }

  function renderNotePageOutlineOpen() {
    const view = state.notesView;
    const body = $("[data-note-page-body]");
    if (body) body.dataset.outlineOpen = view.pageOutlineOpen ? "true" : "false";
    $("[data-note-page-outline-toggle]")?.setAttribute("aria-expanded", view.pageOutlineOpen ? "true" : "false");
  }

  function renderNotePage() {
    const view = state.notesView, data = view.page;
    const page = data?.page || (view.pages || []).find((row) => row.id === view.pageId) || {};
    const canEdit = notePageCanEdit(page), isOwner = page.role === "owner";
    const title = page.title || (data ? "Untitled page" : "Loading page…");
    $("[data-note-page-view]")?.style.setProperty("--page-accent", notePageAccent(page.accent));
    const crumb = $("[data-note-page-crumb]");
    if (crumb) crumb.textContent = title;
    const crumbEmoji = $("[data-note-page-crumb-emoji]");
    if (crumbEmoji) { crumbEmoji.textContent = page.emoji || ""; crumbEmoji.hidden = !page.emoji; }
    const pill = $("[data-note-page-access]");
    if (pill) {
      const label = !page.role ? "" : isOwner ? ((page.shares || []).length ? "Shared" : "Private") : canEdit ? "Can edit" : "View only";
      pill.innerHTML = label ? noteIcon(isOwner ? ((page.shares || []).length ? "users" : "lock") : canEdit ? "pencil" : "eye") + escapeHtml(label) : "";
      pill.hidden = !label;
    }
    $("[data-note-page-customize-open]").hidden = !canEdit;
    $("[data-note-page-share-open]").hidden = !isOwner;
    if (!isOwner) { const panel = $("[data-note-page-share-panel]"); if (panel) panel.hidden = true; }
    const heroEmoji = $("[data-note-page-emoji-view]");
    if (heroEmoji) { heroEmoji.textContent = page.emoji || ""; heroEmoji.hidden = !page.emoji; }
    const heading = $("[data-note-page-title]");
    if (heading) heading.textContent = title;
    const description = $("[data-note-page-description]");
    if (description) {
      const text = String(page.description || "").trim();
      description.innerHTML = text ? renderMarkdown(text) : "";
      description.hidden = !text;
    }
    renderNotePageOutline();
    renderNotePageNotes();
    renderNotePageOutlineOpen();
    if (isOwner) renderNotePageShares();
    refreshNoteIcons();
  }

  function renderNotePageOutline() {
    const view = state.notesView, nav = $("[data-note-page-outline]");
    if (!nav) return;
    const data = view.page;
    if (!data) { nav.innerHTML = ""; return; }
    const page = data.page, notes = data.notes || {};
    const canEdit = notePageCanEdit(page);
    const rows = notePageRows(page.outline, view.pageCollapsed).filter((row) => !row.hidden);
    if (!rows.length) {
      nav.innerHTML = `<p class="notes-pages-empty">Nothing on this page yet.${canEdit ? " Choose Customize to add notes, sections and links." : ""}</p>`;
      return;
    }
    nav.innerHTML = rows.map(({ entry }) => {
      const depth = Math.max(0, Math.min(NOTE_PAGE_MAX_DEPTH, Number(entry.depth || 0)));
      const style = `style="--depth:${depth}"`;
      if (entry.kind === "section") {
        const collapsed = Boolean(view.pageCollapsed.get(entry.id));
        return `<button type="button" class="notes-page-entry" data-kind="section" data-note-page-section="${escapeHtml(entry.id)}" aria-expanded="${collapsed ? "false" : "true"}" ${style}>${noteIcon(collapsed ? "chevron-right" : "chevron-down")}<span class="notes-page-entry-label">${escapeHtml(notePageEntryTitle(entry))}</span></button>`;
      }
      if (entry.kind === "link") {
        const url = /^https:\/\//i.test(entry.url || "") ? entry.url : "";
        return `<a class="notes-page-entry" data-kind="link" data-note-page-link="${escapeHtml(entry.id)}" href="${escapeHtml(url)}" target="_blank" rel="noopener noreferrer" title="${escapeHtml(url)}" ${style}>${noteIcon("external-link")}<span class="notes-page-entry-label">${escapeHtml(notePageEntryTitle(entry))}</span></a>`;
      }
      const note = notes[entry.noteId];
      if (!note) {
        return `<div class="notes-page-entry" data-kind="locked" data-note-page-locked="${escapeHtml(entry.id)}" title="You don't have access to this note" ${style}>${noteIcon("lock")}<span class="notes-page-entry-label">${escapeHtml(String(entry.label || "").trim() || "Locked note")}<span class="notes-page-entry-hint">You don't have access to this note</span></span></div>`;
      }
      const current = view.pageNoteId === entry.noteId;
      const unread = Boolean(note.unread) && !(current && page.layout !== "continuous");
      const emoji = page.showEmoji !== false && note.emoji
        ? `<span class="notes-page-entry-emoji">${escapeHtml(note.emoji)}</span>` : noteIcon("file-text");
      return `<button type="button" class="notes-page-entry" data-kind="note" data-note-page-entry="${escapeHtml(entry.id)}" data-note-page-note="${escapeHtml(entry.noteId)}" aria-current="${current ? "true" : "false"}" ${style}>${emoji}<span class="notes-page-entry-label">${escapeHtml(notePageEntryTitle(entry, note))}</span>${unread ? '<span class="notes-row-dot" title="Changed since you last opened it" aria-label="Unread"></span>' : ""}</button>`;
    }).join("");
    refreshNoteIcons();
  }

  function notePageNoteHtml(entry, note) {
    const markdown = state.notesView.pageContent.get(note.id);
    const body = markdown === undefined ? '<p class="notes-preview-empty">Loading…</p>'
      : markdown === null ? '<p class="notes-preview-empty">This note could not be loaded.</p>'
      : markdown.trim() ? renderMarkdown(markdown, { noteDocument: true }) : '<p class="notes-preview-empty">Nothing written yet.</p>';
    const edited = noteEditedLabel(note);
    return `<article class="notes-page-note" id="note-page-${escapeHtml(entry.id)}" data-note-page-anchor="${escapeHtml(entry.id)}">
        <header class="notes-page-note-head">
          <h2 class="notes-page-note-title">${noteEmojiHtml(note.emoji)}<span>${escapeHtml(notePageEntryTitle(entry, note))}</span></h2>
          <button type="button" class="notes-btn" data-note-page-open-editor="${escapeHtml(note.id)}">${noteIcon("square-pen")}Open in editor</button>
        </header>
        ${edited ? `<p class="notes-page-note-meta">${escapeHtml(edited)}</p>` : ""}
        <div class="notes-preview">${body}</div>
      </article>`;
  }

  function renderNotePageNotes() {
    const view = state.notesView, target = $("[data-note-page-notes]");
    if (!target) return;
    const data = view.page;
    if (!data) { target.innerHTML = '<p class="notes-page-placeholder">Loading page…</p>'; return; }
    const page = data.page, notes = data.notes || {};
    target.dataset.layout = page.layout === "continuous" ? "continuous" : "outline";
    if (page.layout === "continuous") {
      target.innerHTML = page.outline.map((entry) => {
        if (entry.kind === "section") return `<h2 class="notes-page-section" id="note-page-${escapeHtml(entry.id)}" data-note-page-anchor="${escapeHtml(entry.id)}">${escapeHtml(notePageEntryTitle(entry))}</h2>`;
        if (entry.kind !== "note") return "";
        const note = notes[entry.noteId];
        if (!note) return `<p class="notes-page-locked" data-note-page-anchor="${escapeHtml(entry.id)}">${noteIcon("lock")}${escapeHtml(String(entry.label || "").trim() || "Locked note")} · You don't have access to this note</p>`;
        return notePageNoteHtml(entry, note);
      }).join("") || '<p class="notes-page-placeholder">Nothing on this page yet.</p>';
    } else {
      const entry = page.outline.find((item) => item.kind === "note" && item.noteId === view.pageNoteId && notes[item.noteId]);
      target.innerHTML = entry ? notePageNoteHtml(entry, notes[entry.noteId])
        : `<p class="notes-page-placeholder">${page.outline.length ? "Pick a note from the outline." : "Nothing on this page yet."}</p>`;
    }
    refreshNoteIcons();
  }

  function pickNotePageNote(noteId, entryId) {
    const view = state.notesView, page = view.page?.page;
    if (!page) return;
    view.pageNoteId = noteId;
    rememberNotePageInUrl(page.id, noteId);
    if (window.matchMedia(NOTE_PAGE_MOBILE).matches) view.pageOutlineOpen = false;
    renderNotePageOutlineOpen();
    if (page.layout === "continuous") {
      renderNotePageOutline();
      $(`[data-note-page-notes] [data-note-page-anchor="${CSS.escape(entryId)}"]`)?.scrollIntoView({ block: "start", behavior: "smooth" });
      return;
    }
    renderNotePageOutline(); renderNotePageNotes();
    const content = $("[data-note-page-content]");
    if (content) content.scrollTop = 0;
    markNotePageSeen(noteId);
    void fillNotePageContent();
  }

  async function openNoteFromPage(noteId) {
    closeNotePage();
    showNoteEditorOnMobile();
    await selectNote(noteId);
  }

  async function createNotePage() {
    const view = state.notesView;
    noteNotice("");
    try {
      const payload = await noteRequest("/api/notes/pages", "POST", {});
      const page = payload.page;
      view.pages = [notePageSummary(page), ...(view.pages || []).filter((row) => row.id !== page.id)];
      await openNotePage(page.id, { customize: true });
    } catch (error) { noteNotice("main", error.message, "bad"); }
  }

  function notePageDraftFrom(page) {
    return {
      title: page.title || "", emoji: page.emoji || "", description: page.description || "",
      accent: NOTE_PAGE_ACCENTS[page.accent] ? page.accent : "blue",
      layout: page.layout === "continuous" ? "continuous" : "outline",
      showEmoji: page.showEmoji !== false,
      outline: (page.outline || []).map((entry) => ({
        id: entry.id, kind: entry.kind, depth: Number(entry.depth || 0), label: entry.label || "",
        noteId: entry.noteId || "", url: entry.url || "", collapsed: Boolean(entry.collapsed),
      })),
    };
  }

  function openNotePageCustomize() {
    const view = state.notesView, page = view.page?.page;
    if (!page || !notePageCanEdit(page)) return;
    closeNotePopovers();
    view.pageDraft = notePageDraftFrom(page);
    view.pageDraftVersion = page.version;
    $("[data-note-page-conflict]").hidden = true;
    $("[data-note-page-add-box]").hidden = true;
    noteNotice("page-customize", "");
    renderNotePageCustomize();
    $("[data-note-page-customize]").hidden = false;
    $("[data-note-page-customize-open]")?.setAttribute("aria-expanded", "true");
    $('[data-note-page-field="title"]')?.focus();
  }

  function closeNotePageCustomize() {
    const view = state.notesView;
    view.pageDraft = null;
    const drawer = $("[data-note-page-customize]");
    if (drawer) drawer.hidden = true;
    $("[data-note-page-customize-open]")?.setAttribute("aria-expanded", "false");
  }

  function renderNotePageDraftEmoji() {
    const button = $("[data-note-page-emoji]"), emoji = state.notesView.pageDraft?.emoji || "";
    if (!button) return;
    button.innerHTML = emoji ? escapeHtml(emoji) : noteIcon("smile-plus");
    button.dataset.empty = emoji ? "false" : "true";
    button.setAttribute("aria-label", emoji ? `Page emoji ${emoji}. Change emoji` : "Add an emoji");
    button.title = emoji ? "Change emoji" : "Add an emoji";
    refreshNoteIcons();
  }

  function renderNotePageCustomize() {
    const view = state.notesView, draft = view.pageDraft, page = view.page?.page;
    if (!draft || !page) return;
    $('[data-note-page-field="title"]').value = draft.title;
    $('[data-note-page-field="description"]').value = draft.description;
    $('[data-note-page-field="showEmoji"]').checked = draft.showEmoji;
    renderNotePageDraftEmoji();
    $("[data-note-page-accents]").innerHTML = Object.entries(NOTE_PAGE_ACCENTS).map(([name, color]) =>
      `<button type="button" role="radio" class="notes-page-swatch" data-note-page-accent-choice="${name}" aria-checked="${draft.accent === name ? "true" : "false"}" aria-label="${name[0].toUpperCase()}${name.slice(1)}" title="${name[0].toUpperCase()}${name.slice(1)}" style="background:${color}"></button>`).join("");
    document.querySelectorAll("[data-note-page-layout-choice]").forEach((button) =>
      button.setAttribute("aria-pressed", button.dataset.notePageLayoutChoice === draft.layout ? "true" : "false"));
    $("[data-note-page-delete]").hidden = page.role !== "owner";
    renderNotePageEditList();
  }

  function renderNotePageEditList(focus = null) {
    const view = state.notesView, draft = view.pageDraft, list = $("[data-note-page-edit-list]");
    if (!draft || !list) return;
    const notes = view.page?.notes || {};
    const total = draft.outline.length;
    list.innerHTML = draft.outline.map((entry, index) => {
      const note = entry.kind === "note" ? notes[entry.noteId] || view.items.find((item) => item.id === entry.noteId) : null;
      const locked = entry.kind === "note" && !note;
      const kindName = { note: "Note", section: "Section", link: "Link" }[entry.kind] || "Entry";
      const placeholder = entry.kind === "note" ? (note?.title || "A note you can't open") : entry.kind === "section" ? "Section name" : "Link text";
      const icon = entry.kind === "note" && note?.emoji ? escapeHtml(note.emoji)
        : noteIcon(locked ? "lock" : { note: "file-text", section: "heading", link: "link" }[entry.kind] || "dot");
      const tool = (action, iconName, label, disabled = false, pressed = null) =>
        `<button type="button" class="notes-icon-btn" data-note-page-edit-action="${action}" title="${label}" aria-label="${label}"${pressed === null ? "" : ` aria-pressed="${pressed ? "true" : "false"}"`}${disabled ? " disabled" : ""}>${noteIcon(iconName)}</button>`;
      return `<li class="notes-page-edit" data-note-page-edit="${escapeHtml(entry.id)}" data-kind="${escapeHtml(entry.kind)}" style="--depth:${Number(entry.depth || 0)}">
          <span class="notes-page-grip" data-note-page-grip title="Drag to reorder">${noteIcon("grip-vertical")}</span>
          <span class="notes-page-edit-kind" title="${kindName}">${icon}</span>
          <span class="notes-page-edit-fields">
            <input data-note-page-entry-label value="${escapeHtml(entry.label)}" maxlength="200" placeholder="${escapeHtml(placeholder)}" aria-label="${kindName} label" />
            ${entry.kind === "link" ? `<input data-note-page-entry-url value="${escapeHtml(entry.url)}" maxlength="2000" inputmode="url" placeholder="https://" aria-label="Link address" />` : ""}
          </span>
          <span class="notes-page-edit-tools">
            ${entry.kind === "section" ? tool("collapse", "chevrons-down-up", "Start collapsed", false, entry.collapsed) : ""}
            ${tool("outdent", "chevron-left", "Outdent", entry.depth <= 0)}
            ${tool("indent", "chevron-right", "Indent", entry.depth >= NOTE_PAGE_MAX_DEPTH)}
            ${tool("up", "arrow-up", "Move up", index === 0)}
            ${tool("down", "arrow-down", "Move down", index === total - 1)}
            ${tool("remove", "x", "Remove")}
          </span>
        </li>`;
    }).join("") || '<li class="notes-page-edit-empty">No entries yet. Add a note, a section or a link.</li>';
    document.querySelectorAll("[data-note-page-add]").forEach((button) => { button.disabled = total >= NOTE_PAGE_MAX_ENTRIES; });
    if (focus) {
      const row = list.querySelector(`[data-note-page-edit="${CSS.escape(focus.id)}"]`);
      const control = row?.querySelector(focus.selector);
      (control && !control.disabled ? control : row?.querySelector("[data-note-page-entry-label]"))?.focus();
      if (focus.select) control?.select?.();
    }
    refreshNoteIcons();
  }

  function renderNotePageAddResults() {
    const view = state.notesView, target = $("[data-note-page-add-results]");
    if (!target) return;
    const query = String($("[data-note-page-add-search]")?.value || "").trim().toLowerCase();
    const onPage = new Set((view.pageDraft?.outline || []).filter((entry) => entry.kind === "note").map((entry) => entry.noteId));
    const matches = view.items.filter((note) => !query || String(note.title || "").toLowerCase().includes(query)).slice(0, 8);
    target.innerHTML = matches.map((note) => `<button type="button" role="option" data-note-page-add-note="${escapeHtml(note.id)}">
        <span class="notes-page-edit-kind">${note.emoji ? escapeHtml(note.emoji) : noteIcon("file-text")}</span>
        <span class="notes-page-entry-label">${escapeHtml(note.title || "Untitled note")}</span>
        ${onPage.has(note.id) ? '<span class="notes-person-hint">On the page</span>' : ""}
      </button>`).join("") || `<p class="notes-pages-empty">${query ? "No notes match." : "You have no notes yet."}</p>`;
    refreshNoteIcons();
  }

  function addNotePageEntry(entry, selector = "[data-note-page-entry-label]", select = false) {
    const view = state.notesView, draft = view.pageDraft;
    if (!draft) return;
    if (draft.outline.length >= NOTE_PAGE_MAX_ENTRIES) { noteNotice("page-customize", NOTE_ERRORS.too_many_entries, "bad"); return; }
    const full = { id: notePageEntryId(), depth: 0, label: "", noteId: "", url: "", collapsed: false, ...entry };
    draft.outline.push(full);
    noteNotice("page-customize", "");
    renderNotePageEditList({ id: full.id, selector, select });
    $(`[data-note-page-edit="${CSS.escape(full.id)}"]`)?.scrollIntoView({ block: "nearest" });
  }

  function editNotePageEntry(id, action) {
    const draft = state.notesView.pageDraft;
    if (!draft) return;
    const index = draft.outline.findIndex((entry) => entry.id === id);
    if (index < 0) return;
    const entry = draft.outline[index];
    const selector = `[data-note-page-edit-action="${action}"]`;
    if (action === "remove") {
      draft.outline.splice(index, 1);
      const next = draft.outline[Math.min(index, draft.outline.length - 1)];
      renderNotePageEditList(next ? { id: next.id, selector } : null);
      return;
    }
    if (action === "indent") entry.depth = Math.min(NOTE_PAGE_MAX_DEPTH, entry.depth + 1);
    if (action === "outdent") entry.depth = Math.max(0, entry.depth - 1);
    if (action === "collapse") entry.collapsed = !entry.collapsed;
    if (action === "up" && index > 0) draft.outline.splice(index - 1, 0, ...draft.outline.splice(index, 1));
    if (action === "down" && index < draft.outline.length - 1) draft.outline.splice(index + 1, 0, ...draft.outline.splice(index, 1));
    renderNotePageEditList({ id, selector });
  }

  function moveNotePageEntry(id, targetId, after) {
    const draft = state.notesView.pageDraft;
    if (!draft || id === targetId) return;
    const from = draft.outline.findIndex((entry) => entry.id === id);
    if (from < 0) return;
    const [entry] = draft.outline.splice(from, 1);
    const to = draft.outline.findIndex((item) => item.id === targetId);
    draft.outline.splice(to < 0 ? draft.outline.length : to + (after ? 1 : 0), 0, entry);
    renderNotePageEditList();
  }

  async function saveNotePageDraft() {
    const view = state.notesView, page = view.page?.page, draft = view.pageDraft;
    if (!page || !draft || view.pageSaving) return;
    const badLink = draft.outline.find((entry) => entry.kind === "link" && !/^https:\/\/[^\s/]+\S*$/i.test(entry.url.trim()));
    const badSection = draft.outline.find((entry) => entry.kind === "section" && !entry.label.trim());
    const bad = badLink || badSection;
    if (bad) {
      noteNotice("page-customize", badLink ? "Links need an address that starts with https://" : "Give every section a name.", "bad");
      $(`[data-note-page-edit="${CSS.escape(bad.id)}"] ${badLink ? "[data-note-page-entry-url]" : "[data-note-page-entry-label]"}`)?.focus();
      return;
    }
    if (draft.description.length > 2000) { noteNotice("page-customize", "The description can be up to 2000 characters.", "bad"); return; }
    const body = {
      baseVersion: view.pageDraftVersion,
      title: draft.title.trim(), emoji: draft.emoji, description: draft.description,
      accent: draft.accent, layout: draft.layout, showEmoji: draft.showEmoji,
      outline: draft.outline.map((entry) => ({
        id: entry.id, kind: entry.kind, depth: entry.depth, label: entry.label.trim(),
        noteId: entry.kind === "note" ? entry.noteId : "", url: entry.kind === "link" ? entry.url.trim() : "",
        collapsed: entry.kind === "section" && entry.collapsed,
      })),
    };
    view.pageSaving = true;
    const button = $("[data-note-page-save]");
    if (button) button.disabled = true;
    noteNotice("page-customize", "");
    try {
      const payload = await noteRequest(`/api/notes/pages/${encodeURIComponent(page.id)}`, "PATCH", body);
      if (view.pageId !== page.id) return;
      for (const entry of body.outline) if (entry.kind === "section") view.pageCollapsed.set(entry.id, entry.collapsed);
      closeNotePageCustomize();
      if (payload.page && view.page) {
        view.page = { ...view.page, page: { ...view.page.page, ...payload.page, outline: payload.page.outline || body.outline } };
        renderNotePage();
      }
      noteNotice("page", "Page saved");
      await loadNotePage(page.id);
    } catch (error) {
      if (error.status === 409) {
        $("[data-note-page-conflict]").hidden = false;
        noteNotice("page-customize", "");
      } else noteNotice("page-customize", error.message, "bad");
    } finally {
      view.pageSaving = false;
      if (button) button.disabled = false;
    }
  }

  async function deleteNotePage() {
    const view = state.notesView, page = view.page?.page;
    if (!page || page.role !== "owner") return;
    if (!window.confirm(`Delete “${page.title || "Untitled page"}”? The notes on it are kept.`)) return;
    try {
      await noteRequest(`/api/notes/pages/${encodeURIComponent(page.id)}`, "DELETE", {});
      view.pages = (view.pages || []).filter((row) => row.id !== page.id);
      closeNotePage();
      $("[data-note-app]").dataset.mobile = "list";
      noteNotice("main", "Page deleted");
      await loadNotes();
    } catch (error) { noteNotice("page-customize", error.message, "bad"); }
  }

  function renderNotePageShares() {
    const page = state.notesView.page?.page;
    if (!page) return;
    const title = $("[data-note-page-share-title]");
    if (title) title.textContent = page.title || "Untitled page";
    const people = $("[data-note-page-shares]");
    if (people) people.innerHTML = noteShareListHtml(page.shares || [], "note-page");
    const link = $("[data-note-page-access-link]");
    if (link) { link.href = notePageUrl(page.id); link.textContent = notePageUrl(page.id).replace(/^https?:\/\//, ""); }
    refreshNoteIcons();
  }

  async function updateNotePageShare(body, method, message) {
    const view = state.notesView, page = view.page?.page;
    if (!page) return false;
    try {
      const payload = await noteRequest(`/api/notes/pages/${encodeURIComponent(page.id)}/shares`, method, body);
      if (view.page?.page?.id !== page.id) return true;
      view.page.page = { ...view.page.page, shares: payload.shares || [] };
      view.pages = (view.pages || []).map((row) => row.id === page.id ? { ...row, shares: payload.shares || [] } : row);
      renderNotePage();
      noteNotice("page-share", message);
      if ((payload.sharedNotes || []).length) void refreshNoteList();
      return true;
    } catch (error) { noteNotice("page-share", error.message, "bad"); return false; }
  }

  function wireNotePages() {
    const view = state.notesView;
    $("[data-note-pages]")?.addEventListener("click", (event) => {
      if (event.target.closest("[data-note-page-new]")) { void createNotePage(); return; }
      const row = event.target.closest("[data-note-page-id]");
      if (!row) return;
      noteNotice("");
      if (row.dataset.notePageId === view.pageId) { showNoteEditorOnMobile(); return; }
      void openNotePage(row.dataset.notePageId);
    });
    $("[data-note-page-back]")?.addEventListener("click", () => { $("[data-note-app]").dataset.mobile = "list"; });
    $("[data-note-page-outline-toggle]")?.addEventListener("click", () => {
      view.pageOutlineOpen = !view.pageOutlineOpen; renderNotePageOutlineOpen();
    });
    $("[data-note-page-outline]")?.addEventListener("click", (event) => {
      const section = event.target.closest("[data-note-page-section]");
      if (section) {
        const id = section.dataset.notePageSection;
        view.pageCollapsed.set(id, !view.pageCollapsed.get(id));
        renderNotePageOutline();
        $(`[data-note-page-section="${CSS.escape(id)}"]`)?.focus();
        return;
      }
      const entry = event.target.closest("[data-note-page-entry]");
      if (entry) pickNotePageNote(entry.dataset.notePageNote, entry.dataset.notePageEntry);
      if (event.target.closest("[data-note-page-link]") && window.matchMedia(NOTE_PAGE_MOBILE).matches) {
        view.pageOutlineOpen = false; renderNotePageOutlineOpen();
      }
    });
    $("[data-note-page-notes]")?.addEventListener("click", (event) => {
      const open = event.target.closest("[data-note-page-open-editor]");
      if (open) void openNoteFromPage(open.dataset.notePageOpenEditor);
    });

    $("[data-note-page-customize-open]")?.addEventListener("click", () => {
      if ($("[data-note-page-customize]").hidden) openNotePageCustomize(); else closeNotePageCustomize();
    });
    document.querySelectorAll("[data-note-page-customize-close]").forEach((button) =>
      button.addEventListener("click", closeNotePageCustomize));
    $("[data-note-page-emoji]")?.addEventListener("click", (event) => {
      event.stopPropagation(); openNoteEmojiPicker("page", event.currentTarget);
    });
    const drawer = $("[data-note-page-customize]");
    const onField = (event) => {
      const draft = view.pageDraft;
      if (!draft) return;
      const target = event.target;
      if (target.matches("[data-note-page-field]")) {
        const field = target.dataset.notePageField;
        draft[field] = target.type === "checkbox" ? target.checked : target.value;
        return;
      }
      const row = target.closest("[data-note-page-edit]");
      const entry = row && draft.outline.find((item) => item.id === row.dataset.notePageEdit);
      if (entry && target.matches("[data-note-page-entry-label]")) entry.label = target.value;
      if (entry && target.matches("[data-note-page-entry-url]")) entry.url = target.value;
      if (event.type === "input" && target.matches("[data-note-page-add-search]")) renderNotePageAddResults();
    };
    drawer?.addEventListener("input", onField);
    drawer?.addEventListener("change", onField);
    drawer?.addEventListener("click", (event) => {
      const draft = view.pageDraft;
      if (!draft) return;
      const accent = event.target.closest("[data-note-page-accent-choice]");
      if (accent) { draft.accent = accent.dataset.notePageAccentChoice; renderNotePageCustomize(); $(`[data-note-page-accent-choice="${draft.accent}"]`)?.focus(); return; }
      const layout = event.target.closest("[data-note-page-layout-choice]");
      if (layout) {
        draft.layout = layout.dataset.notePageLayoutChoice;
        document.querySelectorAll("[data-note-page-layout-choice]").forEach((button) =>
          button.setAttribute("aria-pressed", button.dataset.notePageLayoutChoice === draft.layout ? "true" : "false"));
        return;
      }
      const add = event.target.closest("[data-note-page-add]");
      if (add) {
        const kind = add.dataset.notePageAdd;
        if (kind === "note") {
          const box = $("[data-note-page-add-box]");
          box.hidden = !box.hidden;
          if (!box.hidden) { $("[data-note-page-add-search]").value = ""; renderNotePageAddResults(); $("[data-note-page-add-search]").focus(); }
        } else if (kind === "section") addNotePageEntry({ kind: "section", label: "New section" }, "[data-note-page-entry-label]", true);
        else addNotePageEntry({ kind: "link", url: "https://" }, "[data-note-page-entry-url]");
        return;
      }
      const pick = event.target.closest("[data-note-page-add-note]");
      if (pick) { addNotePageEntry({ kind: "note", noteId: pick.dataset.notePageAddNote }); renderNotePageAddResults(); return; }
      const action = event.target.closest("[data-note-page-edit-action]");
      if (action) { editNotePageEntry(action.closest("[data-note-page-edit]").dataset.notePageEdit, action.dataset.notePageEditAction); return; }
      if (event.target.closest("[data-note-page-delete]")) { void deleteNotePage(); return; }
      if (event.target.closest("[data-note-page-reload]")) {
        void (async () => { await loadNotePage(view.pageId); openNotePageCustomize(); })();
      }
    });
    drawer?.addEventListener("keydown", (event) => {
      if (!event.target.matches("[data-note-page-add-search]")) return;
      if (event.key === "Enter") {
        event.preventDefault();
        $("[data-note-page-add-results] [data-note-page-add-note]")?.click();
      } else if (event.key === "Escape") {
        event.stopPropagation(); $("[data-note-page-add-box]").hidden = true; $('[data-note-page-add="note"]')?.focus();
      }
    });
    $("[data-note-page-form]")?.addEventListener("submit", (event) => { event.preventDefault(); void saveNotePageDraft(); });
    const list = $("[data-note-page-edit-list]");
    list?.addEventListener("pointerdown", (event) => {
      const grip = event.target.closest("[data-note-page-grip]");
      if (grip) grip.closest("[data-note-page-edit]").draggable = true;
    });
    list?.addEventListener("dragstart", (event) => {
      const row = event.target.closest("[data-note-page-edit]");
      if (!row) return;
      view.pageDrag = row.dataset.notePageEdit;
      row.dataset.dragging = "true";
      event.dataTransfer.effectAllowed = "move";
      event.dataTransfer.setData("text/plain", view.pageDrag);
    });
    list?.addEventListener("dragover", (event) => {
      const row = event.target.closest("[data-note-page-edit]");
      if (!row || !view.pageDrag) return;
      event.preventDefault();
      const box = row.getBoundingClientRect();
      list.querySelectorAll("[data-drop]").forEach((item) => { if (item !== row) delete item.dataset.drop; });
      row.dataset.drop = event.clientY > box.top + box.height / 2 ? "after" : "before";
    });
    list?.addEventListener("drop", (event) => {
      const row = event.target.closest("[data-note-page-edit]");
      if (!row || !view.pageDrag) return;
      event.preventDefault();
      moveNotePageEntry(view.pageDrag, row.dataset.notePageEdit, row.dataset.drop === "after");
      view.pageDrag = "";
    });
    list?.addEventListener("dragend", () => {
      view.pageDrag = "";
      list.querySelectorAll("[data-note-page-edit]").forEach((item) => {
        item.draggable = false; delete item.dataset.drop; delete item.dataset.dragging;
      });
    });

    wireNoteShareField("note-page");
    $("[data-note-page-share-open]")?.addEventListener("click", (event) => {
      event.stopPropagation();
      renderNotePageShares();
      noteNotice("page-share", "");
      toggleNotePopover("[data-note-page-share-panel]", "[data-note-page-share-open]");
    });
    $("[data-note-page-share-form]")?.addEventListener("submit", async (event) => {
      event.preventDefault();
      view.shareScope = "note-page";
      const input = $("[data-note-page-share-name]"), name = input.value.trim().replace(/^@/, "");
      if (!name) { input.focus(); return; }
      view.shareGeneration++; clearTimeout(view.shareTimer); renderNoteShareSuggestions([]);
      const includeNotes = Boolean($("[data-note-page-share-include]")?.checked);
      const added = await updateNotePageShare({
        type: $("[data-note-page-share-type]").value, name, role: $("[data-note-page-share-role]").value, includeNotes,
      }, "POST", includeNotes ? `Shared the page and your notes on it with ${name}` : `Shared the page with ${name}`);
      if (added) { input.value = ""; $("[data-note-page-share-panel]").hidden = false; input.focus(); }
    });
    $("[data-note-page-shares]")?.addEventListener("change", (event) => {
      const select = event.target.closest("[data-note-page-share-role-change]");
      if (!select) return;
      const [type, name] = select.dataset.notePageShareRoleChange.split(":");
      void updateNotePageShare({ type, name, role: select.value }, "POST", `${name} can now ${select.value === "editor" ? "edit" : "view"} the page`);
    });
    $("[data-note-page-shares]")?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-note-page-unshare]");
      if (!button) return;
      const [type, name] = button.dataset.notePageUnshare.split(":");
      void updateNotePageShare({ type, name }, "DELETE", `Removed ${name}`);
    });
    $("[data-note-page-copy-link]")?.addEventListener("click", async () => {
      const page = view.page?.page;
      if (!page) return;
      try {
        await navigator.clipboard.writeText(notePageUrl(page.id));
        noteNotice("page-share", "Link copied");
      } catch (_) { noteNotice("page-share", `Couldn't copy the link: ${notePageUrl(page.id)}`, "bad"); }
    });
  }

  function onNoteEdited() {
    renderNotePreview();
    scheduleNoteSave();
    sendNoteDraft();
    noteTyped();
    if (state.notesView.commentsOpen) {
      clearTimeout(state.notesView.commentsRenderTimer);
      state.notesView.commentsRenderTimer = setTimeout(renderNoteComments, 400);
    }
  }

  function initNotesPage() {
    const view = state.notesView;
    Object.assign(view, {
      filter: "all", linking: false, localDirty: false, savePromise: null,
      saveTimer: 0, conflict: null, peers: new Map(), presenceTimer: 0,
      socketNoteId: "", shareOrgs: null, shareSuggestions: [], shareSuggestionIndex: -1,
      shareGeneration: 0, shareTimer: 0, searchMatches: null, searchGeneration: 0,
      searchTimer: 0, converting: false, activity: null, activityTab: "all",
      activityTimer: 0, activityGeneration: 0, versionsTimer: 0, versionsGeneration: 0, versionShown: null,
      comments: null, commentsOpen: false, commentsTimer: 0, commentsGeneration: 0,
      commentsRenderTimer: 0, commentDraft: null, commentEditing: "", commentFocus: "",
      pendingComment: noteLinkedComment(), mention: null, selectionSource: "",
      lastTypedAt: 0, editingTimer: 0, emojiPending: null, emojiTarget: "note", shareScope: "note",
      pages: null, pagesError: "", pageId: "", page: null, pageNoteId: "", pageContent: new Map(),
      pageFetching: new Set(), pageCollapsed: new Map(), pageGeneration: 0, pageOutlineOpen: false,
      pageDraft: null, pageDraftVersion: 0, pageSaving: false, pageDrag: "",
      live: null, reactionGroups: new Map(), reactionAnchor: null, imageSelection: null, imageLock: true,
      pagesTimer: 0, pagesKey: "", pagesZoom: 1, pagesCount: 0, follow: "", followTagTimer: 0, cursorTimer: 0, sentCursor: "", panelTab: "comments",
      chat: null, chatTimer: 0, chatGeneration: 0, chatSending: false, chatScroll: false,
      assist: null, assistReady: null, assistStatus: false,
      outlineOpen: false, outlineKey: "", outlineCurrent: -1, scrollGeometry: null, scrollLead: "source", scrollFrame: 0,
    });
    try { view.mode = NOTE_MODES.includes(localStorage.getItem(NOTE_MODE_KEY)) ? localStorage.getItem(NOTE_MODE_KEY) : "live"; } catch (_) { view.mode = "live"; }

    let sidePreference = "", sideTab = "";
    try { sidePreference = localStorage.getItem(NOTE_ACTIVITY_KEY) || ""; sideTab = localStorage.getItem(NOTE_SIDE_TAB_KEY) || ""; } catch (_) {}
    setNotePanelTab(NOTE_SIDE_TABS.includes(sideTab) ? sideTab : "activity");
    setNoteCommentsOpen(sidePreference ? sidePreference === "open" : !window.matchMedia(NOTE_ACTIVITY_OVERLAY).matches, false);
    $("[data-note-activity-toggle]")?.addEventListener("click", () => {
      if (noteSideShows("activity") || noteSideShows("history")) setNoteCommentsOpen(false);
      else openNotePanel("activity");
    });
    wireNoteComments();
    wireNoteReactions();
    wireNoteImages();
    wireNoteAssist();
    wireNoteLiveAndChat();
    $("[data-note-panel=\"activity\"]")?.addEventListener("click", (event) => {
      const tab = event.target.closest("[data-note-activity-tab]");
      if (tab) setNoteActivityTab(tab.dataset.noteActivityTab);
      if (event.target.closest("[data-note-activity-history]")) openNotePanel("history");
    });
    document.querySelectorAll("[data-note-new]").forEach((button) => button.addEventListener("click", () => void createNote()));
    $("[data-note-search]")?.addEventListener("input", searchNotes);
    $("[data-note-filters]")?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-note-filter]"); if (button) setNoteFilter(button.dataset.noteFilter);
    });
    $("[data-note-list]")?.addEventListener("click", (event) => {
      const row = event.target.closest("[data-note-id]"); if (!row) return;
      closeNotePage();
      showNoteEditorOnMobile();
      if (row.dataset.noteId !== view.selected?.id) { noteNotice(""); void selectNote(row.dataset.noteId); }
    });
    $("[data-note-back]")?.addEventListener("click", () => {
      void flushNoteSave(); $("[data-note-app]").dataset.mobile = "list";
    });
    $("[data-note-markdown]")?.addEventListener("input", onNoteEdited);
    $("[data-note-title]")?.addEventListener("input", onNoteEdited);
    $("[data-note-mode-group]")?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-note-mode]"); if (button) setNoteMode(button.dataset.noteMode);
    });
    $("[data-note-toolbar]")?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-note-format]"); if (button) formatNoteSelection(button.dataset.noteFormat);
    });
    $("[data-note-save-state]")?.addEventListener("click", (event) => {
      if (event.target.closest("[data-note-save-retry]")) void saveNote();
    });
    $("[data-view=\"notes\"]")?.addEventListener("keydown", (event) => {
      const mod = event.metaKey || event.ctrlKey;
      if (mod && event.key.toLowerCase() === "s") { event.preventDefault(); void flushNoteSave(); return; }
      if (mod && event.altKey && event.code === "KeyM") { event.preventDefault(); commentOnNoteSelection(); return; }
      if (mod && !event.altKey && !event.shiftKey && event.key.toLowerCase() === "j" && view.selected) {
        event.preventDefault();
        if ($("[data-note-assist-panel]")?.hidden === false) closeNoteAssist(); else if (view.assistReady !== false) openNoteAssist(null);
        return;
      }
      if (mod && event.target.matches("[data-note-markdown]") && ["b", "i"].includes(event.key.toLowerCase())) {
        event.preventDefault(); formatNoteSelection(event.key.toLowerCase() === "b" ? "bold" : "italic"); return;
      }
      if (event.key === "Escape") {
        if (view.linking) { view.linking = false; renderNoteLinks(view.selected); return; }
        closeNotePopovers();
        if (view.versionShown && noteSideShows("history")) closeNoteVersion();
        else if (window.matchMedia(NOTE_ACTIVITY_OVERLAY).matches && view.commentsOpen && !view.commentDraft) setNoteCommentsOpen(false, false);
        $("[data-note-comment-float]").hidden = true;
        $("[data-note-assist-float]").hidden = true;
        $("[data-note-react-float]").hidden = true;
        closeNoteReactionPicker();
        closeNoteAssist();
      }
    });
    window.addEventListener("beforeunload", (event) => {
      if (!view.localDirty && !view.savePromise) return;
      void flushNoteSave(); event.preventDefault(); event.returnValue = "";
    });

    // Popovers and the version drawer.
    $("[data-note-share-open]")?.addEventListener("click", (event) => { event.stopPropagation(); toggleNotePopover("[data-note-share-panel]", "[data-note-share-open]"); });
    $("[data-note-menu-open]")?.addEventListener("click", (event) => { event.stopPropagation(); toggleNotePopover("[data-note-menu]", "[data-note-menu-open]"); });
    document.addEventListener("click", (event) => {
      if (!event.target.closest(".notes-pop, [data-note-share-open], [data-note-menu-open], [data-note-presence], [data-note-live], [data-note-emoji-open], [data-note-page-emoji], [data-note-page-share-open]")) closeNotePopovers();
    });
    $("[data-note-versions-open]")?.addEventListener("click", () => {
      closeNotePopovers(); openNotePanel("history");
      document.querySelector("[data-note-panel-tab=\"history\"]")?.focus();
    });
    $("[data-note-panel=\"history\"]")?.addEventListener("click", async (event) => {
      const show = event.target.closest("[data-note-version-show]");
      if (show) { void showNoteVersion(Number(show.dataset.noteVersionShow)); return; }
      if (event.target.closest("[data-note-version-back]")) { closeNoteVersion(); return; }
      const button = event.target.closest("[data-note-restore]"); if (!button) return;
      const note = view.selected, version = Number(button.dataset.noteRestore);
      try {
        await flushNoteSave();
        const payload = await noteRequest(`/api/notes/${note.id}/versions`, "POST", { version, baseVersion: view.selected.version });
        storeNote(payload.note); renderNoteEditor(view.selected);
        noteNotice("versions", `Restored version ${version} as a new version`);
        closeNoteVersion(); void loadNoteVersions();
        scheduleNoteActivity();
      } catch (error) { noteNotice("versions", error.message, "bad"); }
    });

    // Conflicts.
    $("[data-note-conflict-mine]")?.addEventListener("click", () => {
      const latest = view.conflict; if (!latest) return;
      view.conflict = null; $("[data-note-conflict]").hidden = true;
      storeNote({ ...latest });
      renderNoteEditor(latest, { keepDraft: true });
      view.localDirty = true; void saveNote();
    });
    $("[data-note-conflict-theirs]")?.addEventListener("click", () => {
      const latest = view.conflict; if (!latest) return;
      view.conflict = null; view.emojiPending = null; $("[data-note-conflict]").hidden = true;
      storeNote(latest); renderNoteEditor(latest);
    });

    // Sharing and publishing.
    wireNoteShareField("note");
    $("[data-note-share-form]")?.addEventListener("submit", async (event) => {
      event.preventDefault();
      view.shareScope = "note";
      const input = $("[data-note-share-name]"), name = input.value.trim().replace(/^@/, "");
      if (!name) { input.focus(); return; }
      view.shareGeneration++; clearTimeout(view.shareTimer); renderNoteShareSuggestions([]);
      const added = await updateNoteShare({
        type: $("[data-note-share-type]").value, name, role: $("[data-note-share-role]").value,
      }, "POST", `Shared with ${name}. Copy the link below to send it.`);
      if (added) { input.value = ""; $("[data-note-share-panel]").hidden = false; input.focus(); }
    });
    $("[data-note-shares]")?.addEventListener("change", (event) => {
      const select = event.target.closest("[data-note-share-role-change]"); if (!select) return;
      const [type, name] = select.dataset.noteShareRoleChange.split(":");
      void updateNoteShare({ type, name, role: select.value }, "POST",
        `${name} can now ${select.value === "editor" ? "edit" : "view"}`);
    });
    $("[data-note-shares]")?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-note-unshare]"); if (!button) return;
      const [type, name] = button.dataset.noteUnshare.split(":");
      void updateNoteShare({ type, name }, "DELETE", `Removed ${name}`);
    });
    $("[data-note-public]")?.addEventListener("click", async () => {
      const note = view.selected; if (!note || note.role !== "owner") return;
      const publish = note.visibility !== "public";
      await saveNote({ visibility: publish ? "public" : "private" });
      if (view.selected?.visibility === (publish ? "public" : "private")) {
        noteNotice("share", publish ? "Published — anyone with the link can read it" : "Unpublished");
      }
    });
    $("[data-note-copy-link]")?.addEventListener("click", () =>
      void copyNoteLink(`${location.origin}/notes/${view.selected.id}`, "share", "Couldn't copy — select the link and copy it manually."));
    $("[data-note-copy-access-link]")?.addEventListener("click", () =>
      void copyNoteLink(noteAccessUrl(view.selected), "share", "Couldn't copy — select the link and copy it manually."));
    $("[data-note-menu-copy-link]")?.addEventListener("click", () => {
      closeNotePopovers();
      const note = view.selected; if (!note) return;
      void copyNoteLink(noteAccessUrl(note), "main", `Couldn't copy the link: ${noteAccessUrl(note)}`);
    });

    // Linked conversations.
    document.querySelectorAll("[data-note-menu] [data-note-link-start]").forEach((item) => item.addEventListener("click", startNoteLink));
    $("[data-note-links]")?.addEventListener("click", async (event) => {
      if (event.target.closest("[data-note-link-start]")) { startNoteLink(); return; }
      const button = event.target.closest("[data-note-unlink]"); if (!button) return;
      const note = view.selected;
      try {
        const payload = await noteRequest(`/api/notes/${note.id}/links`, "DELETE", JSON.parse(button.dataset.noteUnlink));
        storeNote({ ...view.selected, links: payload.links || [] });
        renderNoteLinks(view.selected);
        scheduleNoteActivity();
      } catch (error) { noteNotice("links", error.message, "bad"); }
    });
    $("[data-note-links]")?.addEventListener("submit", (event) => {
      if (!event.target.matches("[data-note-link-form]")) return;
      event.preventDefault(); void addNoteLink(event.target);
    });
    $("[data-note-links]")?.addEventListener("keydown", (event) => {
      if (event.target.matches("[data-note-link-input]") && event.key === "Enter") {
        event.preventDefault(); void addNoteLink(event.target.closest("form"));
      }
    });

    $("[data-note-to-task]")?.addEventListener("click", () => void convertNoteToTask());
    $("[data-note-to-issue]")?.addEventListener("click", askNoteIssueRepository);
    document.querySelectorAll("[data-note-issue-cancel]").forEach((button) =>
      button.addEventListener("click", () => $("[data-note-issue-dialog]").close()));
    $("[data-note-issue-form]")?.addEventListener("submit", (event) => {
      event.preventDefault();
      const input = $("[data-note-issue-repo]");
      const parts = input.value.trim().split("/");
      if (parts.length !== 2 || !parts[0] || !parts[1]) {
        noteNotice("issue", "Choose a repository as owner/name.", "bad"); input.focus(); return;
      }
      $("[data-note-issue-dialog]").close();
      void convertNoteToIssue({ owner: parts[0], name: parts[1] });
    });

    // Export and delete.
    $("[data-note-export]")?.addEventListener("click", () => {
      closeNotePopovers();
      const note = view.selected; if (!note) return;
      const title = $("[data-note-title]").value || "Untitled note";
      const blob = new Blob([`# ${title}\n\n${$("[data-note-markdown]").value}`], { type: "text/markdown" });
      const link = document.createElement("a");
      link.href = URL.createObjectURL(blob);
      link.download = `${title.replace(/[^\w.-]+/g, "-").replace(/^-+|-+$/g, "").toLowerCase() || "note"}.md`;
      link.click();
      setTimeout(() => URL.revokeObjectURL(link.href), 1000);
      void recordNoteActivity("export");
    });
    $("[data-note-export-pdf]")?.addEventListener("click", openNotePdfDialog);
    $("[data-note-pdf-form]")?.addEventListener("submit", (event) => {
      event.preventDefault();
      exportNotePdf();
    });
    document.querySelectorAll("[data-note-pdf-cancel]").forEach((button) =>
      button.addEventListener("click", () => $("[data-note-pdf-dialog]").close()));
    $("[data-note-delete]")?.addEventListener("click", () => {
      closeNotePopovers();
      const note = view.selected; if (!note) return;
      $("[data-note-delete-copy]").textContent = `“${note.title || "Untitled note"}” and its version history will be removed for everyone it's shared with. This can't be undone.`;
      $("[data-note-delete-dialog]").showModal();
    });
    $("[data-note-delete-dialog]")?.addEventListener("close", async (event) => {
      if (event.target.returnValue !== "delete") return;
      const note = view.selected; if (!note) return;
      try {
        clearTimeout(view.saveTimer); view.localDirty = false;
        await noteRequest(`/api/notes/${note.id}`, "DELETE", {});
        view.items = view.items.filter((item) => item.id !== note.id);
        view.selected = null;
        $("[data-note-app]").dataset.mobile = "list";
        noteNotice("main", "Note deleted");
        await loadNotes();
      } catch (error) { noteNotice("main", error.message, "bad"); }
    });

    wireNoteEmoji();
    wireNotePages();
    renderNoteList();
    renderNotePages();
    const linkedPage = notePageLinkedId();
    if (linkedPage) {
      view.pageId = linkedPage; view.pageNoteId = noteLinkedId();
      renderNoteMain(); renderNotePage(); showNoteEditorOnMobile();
      void loadNotePage(linkedPage);
      void loadNotes();
    } else void loadNotes(noteLinkedId());
    void loadNotePages();
    document.addEventListener("visibilitychange", () => { if (!document.hidden) void refreshNoteList(); });
    view.listTimer = setInterval(() => void refreshNoteList(), 60000);
  }

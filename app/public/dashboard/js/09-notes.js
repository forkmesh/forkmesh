  // Collaborative Markdown notes. Edits autosave through the version-checked
  // PATCH; the socket only announces persisted changes, in-progress drafts,
  // and lightweight presence.
  const NOTE_AUTOSAVE_MS = 1500;
  const NOTE_PRESENCE_MS = 15000;
  const NOTE_MODE_KEY = "forkmesh.notes.mode";
  const NOTE_SEARCH_MS = 250;
  const NOTE_ACTIVITY_KEY = "forkmesh.notes.activity";
  const NOTE_ACTIVITY_REFRESH_MS = 1200;
  const NOTE_EDIT_GROUP_MS = 30 * 60 * 1000;
  const NOTE_ACTIVITY_OVERLAY = "(max-width: 1279px)";
  const NOTE_HISTORY_KINDS = ["edit", "create", "restore"];
  const NOTE_ACTIVITY_ICONS = {
    view: "eye", web: "globe", edit: "pencil", create: "file-plus", restore: "rotate-ccw",
    share: "user-plus", role: "user-cog", unshare: "user-minus", publish: "globe", unpublish: "eye-off",
    link: "link", unlink: "unlink", export: "download", "copy-link": "copy", task: "list-todo", issue: "circle-dot",
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
  };

  async function noteRequest(path, method = "GET", body = null) {
    const headers = { accept: "application/json" };
    const token = state.session?.sessionToken || "";
    if (token) headers.authorization = `Bearer ${token}`;
    if (body !== null) headers["content-type"] = "application/json";
    const response = await fetch(path, {
      method, headers, cache: "no-store",
      body: body === null ? undefined : JSON.stringify(body),
    });
    const payload = await response.json().catch(() => ({}));
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

  function rememberNoteInUrl(note) {
    if (!location.pathname.startsWith("/dashboard/notes")) return;
    const url = new URL(location.href);
    if (url.searchParams.get("note") === note.id) return;
    url.searchParams.set("note", note.id);
    history.replaceState(history.state, "", url.pathname + url.search + url.hash);
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

  function renderNoteRow(note) {
    const isSelected = state.notesView.selected?.id === note.id;
    const status = noteStatusLabel(note);
    // Owners see who else can read the note; collaborators see what they
    // are allowed to do with someone else's note.
    const access = note.role === "owner"
      ? noteSharedWithLabel(note)
      : `You can ${note.role === "editor" ? "edit" : "view"}`;
    return `
      <button type="button" data-note-id="${escapeHtml(note.id)}" aria-selected="${isSelected ? "true" : "false"}" class="notes-row">
        <span class="notes-row-title">${escapeHtml(note.title || "Untitled note")}</span>
        <span class="notes-row-vis" data-visibility="${note.visibility === "public" ? "public" : "private"}" title="${escapeHtml(status)}" aria-label="${escapeHtml(status)}">${noteIcon(noteVisibilityIcon(note))}</span>
        <span class="notes-row-line" title="${escapeHtml(access)}">${escapeHtml(access)}</span>
        <span class="notes-row-meta"><span class="notes-row-when">${escapeHtml(relativeTimeLabel(note.updatedAt))}</span>${noteStatsHtml(note)}</span>
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
        (groups.length > 1 ? `<div class="notes-group">${label}</div>` : "")
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
    const heading = $("[data-note-preview-title]");
    if (heading) heading.textContent = title;
    const crumb = $("[data-note-crumb]");
    if (crumb) crumb.textContent = title;
    const preview = $("[data-note-preview]");
    if (preview) {
      preview.innerHTML = markdown.trim()
        ? renderMarkdown(markdown)
        : '<p class="notes-preview-empty">Nothing written yet.</p>';
    }
    const words = markdown.split(/\s+/).filter(Boolean).length;
    const counts = $("[data-note-counts]");
    if (counts) counts.textContent = `${words} ${words === 1 ? "word" : "words"} · ${Math.max(1, Math.round(words / 220))} min read · Markdown`;
  }

  function renderNoteMode(note) {
    const canEdit = noteCanEdit(note);
    const mode = canEdit ? state.notesView.mode : "preview";
    const work = $("[data-note-work]");
    if (work) work.dataset.mode = mode;
    const group = $("[data-note-mode-group]");
    if (group) group.hidden = !canEdit;
    document.querySelectorAll("[data-note-mode]").forEach((button) =>
      button.setAttribute("aria-pressed", button.dataset.noteMode === mode ? "true" : "false"));
  }

  function renderNoteShares(note) {
    const shareTitle = $("[data-note-share-title]");
    if (shareTitle) shareTitle.textContent = note.title || "Untitled note";
    const owner = state.session?.nodeName || "You";
    const people = $("[data-note-shares]");
    if (people) {
      people.innerHTML = `
        <div class="notes-person">
          <span class="notes-avatar" style="background:${noteAvatarColor(owner)}">${escapeHtml(noteInitials(owner))}</span>
          <div class="min-w-0"><div class="notes-person-name">${escapeHtml(owner)}</div><div class="notes-person-hint">You</div></div>
          <span class="notes-person-hint">Owner</span><span></span>
        </div>` + (note.shares || []).map((share) => {
        const key = `${escapeHtml(share.type)}:${escapeHtml(share.name)}`;
        return `
        <div class="notes-person">
          <span class="notes-avatar" data-type="${escapeHtml(share.type)}" style="background:${noteAvatarColor(share.name)}">${escapeHtml(noteInitials(share.name))}</span>
          <div class="min-w-0"><div class="notes-person-name">${escapeHtml(share.name)}</div><div class="notes-person-hint">${share.type === "organization" ? "Organization" : "User"}</div></div>
          <select data-note-share-role-change="${key}" aria-label="Access for ${escapeHtml(share.name)}">
            <option value="editor" ${share.role === "editor" ? "selected" : ""}>Can edit</option>
            <option value="viewer" ${share.role !== "editor" ? "selected" : ""}>Can view</option>
          </select>
          <button type="button" data-note-unshare="${key}" class="notes-chip-x" title="Remove access" aria-label="Remove ${escapeHtml(share.name)}">${noteIcon("x")}</button>
        </div>`;
      }).join("");
    }
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
      const section = link.kind === "pull" ? "pulls" : `${link.kind}s`;
      const href = `${repoPathUrl({ owner: link.owner, name: link.repo })}?tab=${section}#${link.number}`;
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

  function renderNoteEditor(note, { keepDraft = false } = {}) {
    const view = state.notesView;
    $("[data-note-empty]").hidden = Boolean(note);
    $("[data-note-editor]").hidden = !note;
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
    const pill = $("[data-note-visibility]");
    pill.dataset.visibility = note.visibility === "public" ? "public" : "private";
    pill.innerHTML = noteIcon(noteVisibilityIcon(note)) + escapeHtml(note.visibility === "public" ? "Published" : noteStatusLabel(note));
    $("[data-note-share-open]").hidden = !isOwner;
    if (!isOwner) closeNotePopovers();
    document.querySelectorAll("[data-note-menu] [data-note-link-start]").forEach((item) => { item.hidden = !canEdit; });
    $("[data-note-delete]").hidden = !isOwner;
    $("[data-note-delete-sep]").hidden = !isOwner;
    renderNoteMode(note);
    renderNotePreview();
    renderNoteShares(note);
    renderNoteLinks(note);
    renderNoteList();
    connectNoteSocket(note);
    if (!$("[data-note-versions-panel]").hidden) void loadNoteVersions();
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
      default: return escapeHtml(entry.kind);
    }
  }

  function noteActivityActor(entry) {
    if (entry.kind === "web") return "";
    const me = String(state.session?.nodeName || "").toLowerCase();
    const actor = String(entry.actor || "");
    return `<b>${escapeHtml(actor && actor === me ? "You" : actor || "Someone")}</b> `;
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

  function setNoteActivityOpen(open, remember = true) {
    const view = state.notesView;
    view.activityOpen = open;
    if (remember) { try { localStorage.setItem(NOTE_ACTIVITY_KEY, open ? "open" : "closed"); } catch (_) {} }
    const panel = $("[data-note-activity]");
    if (panel) panel.hidden = !open;
    const toggle = $("[data-note-activity-toggle]");
    toggle?.setAttribute("aria-pressed", open ? "true" : "false");
    toggle?.setAttribute("aria-label", open ? "Hide activity" : "Show activity");
    if (open) renderNoteActivity();
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
      if (!same) { view.conflict = null; $("[data-note-conflict]").hidden = true; }
      storeNote(payload.note);
      rememberNoteInUrl(payload.note);
      renderNoteEditor(view.selected, { keepDraft: keepDraft && same && view.localDirty });
      if (same) scheduleNoteActivity();
      else { view.activity = null; renderNoteActivity(); void recordNoteActivity("view"); }
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
    if (!view.localDirty && !("visibility" in extra)) return;
    const title = $("[data-note-title]").value, markdown = $("[data-note-markdown]").value;
    setNoteSaveState("saving");
    try {
      const payload = await noteRequest(`/api/notes/${note.id}`, "PATCH", {
        baseVersion: note.version, title, markdown, ...extra,
      });
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

  async function loadNoteVersions() {
    const note = state.notesView.selected; if (!note) return;
    const target = $("[data-note-versions]");
    try {
      const payload = await noteRequest(`/api/notes/${note.id}/versions`);
      const canEdit = noteCanEdit(note);
      target.innerHTML = (payload.versions || []).map((version) => {
        const current = version.version === note.version;
        return `
        <div class="notes-version" data-current="${current ? "true" : "false"}">
          <span class="notes-version-node"></span>
          <div><div class="notes-version-name">Version ${version.version}</div><div class="notes-version-hint">${escapeHtml(relativeTimeLabel(version.createdAt))}${version.author ? ` · by @${escapeHtml(version.author)}` : ""}</div></div>
          ${current ? '<span class="notes-version-tag">Current</span>'
            : canEdit ? `<button type="button" data-note-restore="${version.version}">Restore</button>` : "<span></span>"}
        </div>`;
      }).join("") || '<p class="p-3 text-xs text-muted-foreground">No saved versions yet.</p>';
    } catch (error) {
      target.innerHTML = `<div class="fm-flash fm-flash-danger"><i data-lucide="circle-alert"></i><div class="fm-flash-body">${escapeHtml(error.message)}</div></div>`;
      refreshNoteIcons();
    }
  }

  function sendNotePresence() {
    const socket = state.notesView.socket;
    if (socket?.readyState !== WebSocket.OPEN) return;
    socket.send(JSON.stringify({ type: "presence", clientId: state.notesView.clientId }));
  }

  function renderNotePresence() {
    const view = state.notesView;
    const now = Date.now();
    for (const [id, peer] of view.peers) {
      if (now - peer.seen > NOTE_PRESENCE_MS * 2.5) view.peers.delete(id);
    }
    const peers = [...view.peers.values()];
    const faces = $("[data-note-presence]");
    if (faces) {
      faces.innerHTML = peers.slice(0, 4).map((peer) =>
        `<span class="notes-face" style="background:${noteAvatarColor(peer.name)}" title="${escapeHtml(peer.name)} is here">${escapeHtml(noteInitials(peer.name))}</span>`).join("");
    }
    const live = $("[data-note-live]");
    if (!live) return;
    const open = view.socket?.readyState === WebSocket.OPEN;
    const editing = peers.find((peer) => now - (peer.draftAt || 0) < 5000);
    live.textContent = !open ? "Offline — edits still save"
      : editing ? `${editing.name} is editing…`
      : peers.length ? `Live · ${peers.length === 1 ? `${peers[0].name} is here` : `${peers.length} others here`}`
      : "Live";
  }

  function connectNoteSocket(note) {
    const view = state.notesView;
    if (view.socket && view.socketNoteId === note.id && view.socket.readyState <= WebSocket.OPEN) return;
    view.socket?.close(); view.socket = null;
    view.peers = new Map();
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
        view.peers.set(frame.clientId, { ...peer, name: String(frame.name || "Collaborator").slice(0, 32), seen: Date.now() });
        // Answer a newcomer once so they see who was already here.
        if (!known) sendNotePresence();
      }
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
      if (frame.type === "note-updated" && frame.version > (view.selected?.version || 0) && !view.savePromise) {
        // With unsaved local edits the next autosave meets the 409 and asks.
        if (!view.localDirty) void selectNote(note.id);
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
        markdown: $("[data-note-markdown]").value,
      }));
    }, 180);
  }

  function closeNotePopovers() {
    for (const [panel, trigger] of [["[data-note-share-panel]", "[data-note-share-open]"], ["[data-note-menu]", "[data-note-menu-open]"]]) {
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
    if (state.notesView.selected) renderNoteMode(state.notesView.selected);
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
    if ($("[data-note-work]").dataset.mode === "preview") setNoteMode("split");
    state.notesView.linking = true;
    renderNoteLinks(state.notesView.selected);
    $("[data-note-link-input]")?.focus();
  }

  function noteShareQuery() {
    return ($("[data-note-share-name]")?.value || "").trim().replace(/^@/, "").toLowerCase();
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
    const view = state.notesView;
    const list = $("[data-note-share-suggestions]"), input = $("[data-note-share-name]");
    if (!list || !input) return;
    view.shareSuggestions = items; view.shareSuggestionIndex = index;
    const open = items.length > 0 && document.activeElement === input;
    list.innerHTML = open ? items.map((item, position) => `
      <button type="button" role="option" id="note-share-option-${position}" tabindex="-1" data-note-share-option="${position}" aria-selected="${position === index ? "true" : "false"}">
        <span class="notes-avatar" data-type="${escapeHtml(item.type)}" style="background:${noteAvatarColor(item.name)}">${escapeHtml(noteInitials(item.name))}</span>
        <span class="notes-person-name">${escapeHtml(item.name)}</span>
        <span class="notes-person-hint">${item.type === "organization" ? "Organization" : "User"}</span>
      </button>`).join("") : "";
    list.hidden = !open;
    input.setAttribute("aria-expanded", open ? "true" : "false");
    if (open && index >= 0) {
      input.setAttribute("aria-activedescendant", `note-share-option-${index}`);
      list.querySelector(`#note-share-option-${index}`)?.scrollIntoView({ block: "nearest" });
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
      const taken = new Set((view.selected?.shares || []).map((share) => `${share.type}:${String(share.name).toLowerCase()}`));
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
    const input = $("[data-note-share-name]"); if (!input || !item) return;
    input.value = item.name;
    $("[data-note-share-type]").value = item.type;
    state.notesView.shareGeneration++;
    clearTimeout(state.notesView.shareTimer);
    renderNoteShareSuggestions([]);
    input.focus();
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
    const area = $("[data-note-markdown]"); if (!area || area.readOnly) return;
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

  function onNoteEdited() {
    renderNotePreview();
    scheduleNoteSave();
    sendNoteDraft();
  }

  function initNotesPage() {
    const view = state.notesView;
    Object.assign(view, {
      filter: "all", linking: false, localDirty: false, savePromise: null,
      saveTimer: 0, conflict: null, peers: new Map(), presenceTimer: 0,
      socketNoteId: "", shareOrgs: null, shareSuggestions: [], shareSuggestionIndex: -1,
      shareGeneration: 0, shareTimer: 0, searchMatches: null, searchGeneration: 0,
      searchTimer: 0, converting: false, activity: null, activityTab: "all",
      activityTimer: 0, activityGeneration: 0, activityOpen: false,
    });
    try { view.mode = ["write", "split", "preview"].includes(localStorage.getItem(NOTE_MODE_KEY)) ? localStorage.getItem(NOTE_MODE_KEY) : "split"; } catch (_) { view.mode = "split"; }

    let activityPreference = "";
    try { activityPreference = localStorage.getItem(NOTE_ACTIVITY_KEY) || ""; } catch (_) {}
    setNoteActivityOpen(activityPreference ? activityPreference === "open" : !window.matchMedia(NOTE_ACTIVITY_OVERLAY).matches, false);
    $("[data-note-activity-toggle]")?.addEventListener("click", () => setNoteActivityOpen(!view.activityOpen));
    $("[data-note-activity-close]")?.addEventListener("click", () => setNoteActivityOpen(false));
    $("[data-note-activity]")?.addEventListener("click", (event) => {
      const tab = event.target.closest("[data-note-activity-tab]");
      if (tab) setNoteActivityTab(tab.dataset.noteActivityTab);
      if (event.target.closest("[data-note-activity-history]")) {
        $("[data-note-versions-panel]").hidden = false; void loadNoteVersions();
        $("[data-note-versions-close]")?.focus();
      }
    });
    document.querySelectorAll("[data-note-new]").forEach((button) => button.addEventListener("click", () => void createNote()));
    $("[data-note-search]")?.addEventListener("input", searchNotes);
    $("[data-note-filters]")?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-note-filter]"); if (button) setNoteFilter(button.dataset.noteFilter);
    });
    $("[data-note-list]")?.addEventListener("click", (event) => {
      const row = event.target.closest("[data-note-id]"); if (!row) return;
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
      if (mod && event.target.matches("[data-note-markdown]") && ["b", "i"].includes(event.key.toLowerCase())) {
        event.preventDefault(); formatNoteSelection(event.key.toLowerCase() === "b" ? "bold" : "italic"); return;
      }
      if (event.key === "Escape") {
        if (view.linking) { view.linking = false; renderNoteLinks(view.selected); return; }
        closeNotePopovers(); $("[data-note-versions-panel]").hidden = true;
        if (window.matchMedia(NOTE_ACTIVITY_OVERLAY).matches && view.activityOpen) setNoteActivityOpen(false, false);
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
      if (!event.target.closest(".notes-pop, [data-note-share-open], [data-note-menu-open]")) closeNotePopovers();
    });
    $("[data-note-versions-open]")?.addEventListener("click", () => {
      closeNotePopovers(); $("[data-note-versions-panel]").hidden = false; void loadNoteVersions();
      $("[data-note-versions-close]")?.focus();
    });
    $("[data-note-versions-close]")?.addEventListener("click", () => { $("[data-note-versions-panel]").hidden = true; });
    $("[data-note-versions]")?.addEventListener("click", async (event) => {
      const button = event.target.closest("[data-note-restore]"); if (!button) return;
      const note = view.selected, version = Number(button.dataset.noteRestore);
      try {
        await flushNoteSave();
        const payload = await noteRequest(`/api/notes/${note.id}/versions`, "POST", { version, baseVersion: view.selected.version });
        storeNote(payload.note); renderNoteEditor(view.selected);
        noteNotice("versions", `Restored version ${version} as a new version`);
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
      view.conflict = null; $("[data-note-conflict]").hidden = true;
      storeNote(latest); renderNoteEditor(latest);
    });

    // Sharing and publishing.
    const shareName = $("[data-note-share-name]");
    shareName?.addEventListener("input", searchNoteShareCandidates);
    shareName?.addEventListener("focus", () => { if (noteShareQuery()) searchNoteShareCandidates(); });
    shareName?.addEventListener("blur", () => { view.shareGeneration++; renderNoteShareSuggestions([]); });
    shareName?.addEventListener("keydown", (event) => {
      const items = view.shareSuggestions || [];
      if ($("[data-note-share-suggestions]").hidden || !items.length) return;
      if (event.key === "ArrowDown" || event.key === "ArrowUp") {
        event.preventDefault();
        const step = event.key === "ArrowDown" ? 1 : -1;
        renderNoteShareSuggestions(items, (view.shareSuggestionIndex + step + items.length) % items.length);
      } else if (event.key === "Escape") {
        event.stopPropagation(); view.shareGeneration++; renderNoteShareSuggestions([]);
      } else if ((event.key === "Enter" || event.key === "Tab") && view.shareSuggestionIndex >= 0) {
        const item = items[view.shareSuggestionIndex];
        const exact = item.name === noteShareQuery() && item.type === $("[data-note-share-type]").value;
        if (event.key === "Tab" || !exact) event.preventDefault();
        chooseNoteShareCandidate(item);
      }
    });
    $("[data-note-share-suggestions]")?.addEventListener("mousedown", (event) => event.preventDefault());
    $("[data-note-share-suggestions]")?.addEventListener("click", (event) => {
      const option = event.target.closest("[data-note-share-option]"); if (!option) return;
      chooseNoteShareCandidate(view.shareSuggestions[Number(option.dataset.noteShareOption)]);
    });
    $("[data-note-share-form]")?.addEventListener("submit", async (event) => {
      event.preventDefault();
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

    renderNoteList();
    void loadNotes(noteLinkedId());
  }

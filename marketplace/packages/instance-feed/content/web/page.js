window.ForkMeshDashboard.registerPage("instance-feed", ({ select: $, selectAll: $$, session, escapeHtml, relativeTimeLabel }) => {
  const PACKAGE = "instance-feed";
  const API = "/api/instance-feed";
  const MAX_LENGTH = 500;
  const state = { installed: false, loading: false, view: viewFromUrl(), posts: [], thread: null, profile: null, next: "", replyTo: null, revealed: new Set() };

  function viewFromUrl() {
    const params = new URLSearchParams(location.search);
    const post = String(params.get("post") || "");
    const author = String(params.get("author") || "").toLowerCase();
    if (/^[0-9a-f]{32}$/.test(post)) return { kind: "thread", post };
    if (/^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(author)) return { kind: "profile", author };
    return { kind: "timeline" };
  }
  function urlFor(view) {
    const url = new URL(location.href);
    url.searchParams.delete("post"); url.searchParams.delete("author");
    if (view.kind === "thread") url.searchParams.set("post", view.post);
    if (view.kind === "profile") url.searchParams.set("author", view.author);
    return url.pathname + url.search;
  }
  function message(kind, value = "") {
    const node = $(`[data-ifeed-${kind}]`); if (!node) return;
    const slot = node.querySelector("[data-ifeed-message-text]") || node;
    slot.textContent = value; node.classList.toggle("hidden", !value);
  }
  async function request(path, method = "GET", body = null) {
    const headers = { accept: "application/json" };
    const token = session()?.sessionToken;
    if (token) headers.authorization = `Bearer ${token}`;
    if (body !== null) headers["content-type"] = "application/json";
    const response = await fetch(path, { method, headers, cache: "no-store", body: body === null ? undefined : JSON.stringify(body) });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(String(payload.error || `HTTP ${response.status}`).replaceAll("_", " "));
      error.status = response.status; error.payload = payload; throw error;
    }
    return payload;
  }
  function downloadJson(payload, filename) {
    const url = URL.createObjectURL(new Blob([JSON.stringify(payload, null, 2)], { type: "application/json" }));
    const link = document.createElement("a"); link.href = url; link.download = filename; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  function richText(text) {
    const pattern = /(https:\/\/[^\s<>"']+[^\s<>"'.,;:!?)\]])|(^|[^A-Za-z0-9_@/])@([A-Za-z](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)\b/g;
    let html = "", last = 0;
    for (const match of String(text || "").matchAll(pattern)) {
      html += escapeHtml(String(text).slice(last, match.index));
      if (match[1]) {
        html += `<a class="text-accent hover:underline" href="${escapeHtml(match[1])}" target="_blank" rel="noopener noreferrer nofollow">${escapeHtml(match[1])}</a>`;
      } else {
        html += `${escapeHtml(match[2])}<a class="text-accent hover:underline" href="${escapeHtml(urlFor({ kind: "profile", author: match[3].toLowerCase() }))}" data-ifeed-author="${escapeHtml(match[3].toLowerCase())}">@${escapeHtml(match[3])}</a>`;
      }
      last = match.index + match[0].length;
    }
    return html + escapeHtml(String(text || "").slice(last));
  }
  function avatar(name, large = false) {
    const size = large ? "h-12 w-12 text-base" : "h-10 w-10 text-sm";
    return `<span aria-hidden="true" class="flex ${size} shrink-0 items-center justify-center rounded-full border border-border bg-subtle font-semibold uppercase text-muted-foreground">${escapeHtml(String(name || "?").slice(0, 1))}</span>`;
  }
  function timeLabel(ms) {
    return ms ? `<time class="shrink-0 text-xs text-muted-foreground" datetime="${new Date(ms).toISOString()}" title="${escapeHtml(new Date(ms).toLocaleString())}">${escapeHtml(relativeTimeLabel(ms))}</time>` : "";
  }
  function action(kind, post, icon, label, count, active, activeClass) {
    return `<button type="button" data-ifeed-action="${kind}" data-ifeed-id="${escapeHtml(post.id)}" aria-pressed="${active ? "true" : "false"}" aria-label="${escapeHtml(label)}" title="${escapeHtml(label)}" class="fm-btn fm-btn-sm fm-btn-invisible"><span class="flex items-center gap-1 ${active ? activeClass : ""}"><i data-lucide="${icon}"></i><span class="tabular-nums">${count ? escapeHtml(String(count)) : ""}</span></span></button>`;
  }
  function postCard(post, { boostedBy = null, focused = false } = {}) {
    const profileHref = escapeHtml(urlFor({ kind: "profile", author: post.author }));
    const threadHref = escapeHtml(urlFor({ kind: "thread", post: post.id }));
    const hidden = post.spoiler && !state.revealed.has(post.id);
    const body = `<div class="${focused ? "text-base leading-7" : "text-sm leading-6"} whitespace-pre-wrap break-words text-foreground">${richText(post.text)}</div>`;
    const content = post.spoiler
      ? `<div class="mt-1 flex flex-wrap items-center gap-2"><p class="text-sm text-foreground">${escapeHtml(post.spoiler)}</p><button type="button" data-ifeed-reveal="${escapeHtml(post.id)}" class="fm-btn fm-btn-sm">${hidden ? "Show more" : "Show less"}</button></div>${hidden ? "" : `<div class="mt-2">${body}</div>`}`
      : `<div class="mt-1">${body}</div>`;
    const banner = boostedBy ? `<p class="mb-2 flex items-center gap-1 pl-12 text-xs text-muted-foreground"><i data-lucide="repeat-2" class="h-3.5 w-3.5"></i><a class="hover:underline" href="${escapeHtml(urlFor({ kind: "profile", author: boostedBy.name }))}" data-ifeed-author="${escapeHtml(boostedBy.name)}">${escapeHtml(boostedBy.display)}</a><span>boosted</span></p>` : "";
    const replying = post.replyTo ? `<p class="text-xs text-muted-foreground">Replying to ${post.replyToAuthor ? `<a class="text-accent hover:underline" href="${escapeHtml(urlFor({ kind: "profile", author: post.replyToAuthor }))}" data-ifeed-author="${escapeHtml(post.replyToAuthor)}">@${escapeHtml(post.replyToAuthor)}</a>` : "a removed post"}</p>` : "";
    const actions = [
      action("reply", post, "reply", "Reply", post.replies, false, ""),
      action("boost", post, "repeat-2", post.boosted ? "Undo boost" : "Boost", post.boosts, post.boosted, "text-accent"),
      action("favourite", post, "star", post.favourited ? "Remove favourite" : "Favourite", post.favourites, post.favourited, "text-attention"),
      post.canDelete ? `<button type="button" data-ifeed-action="delete" data-ifeed-id="${escapeHtml(post.id)}" class="fm-btn fm-btn-sm fm-btn-invisible ml-auto text-muted-foreground hover:text-destructive" aria-label="Delete post" title="Delete post"><i data-lucide="trash-2"></i></button>` : "",
    ].join("");
    return `<article class="px-4 py-3 ${focused ? "bg-subtle" : ""}" data-ifeed-post="${escapeHtml(post.id)}">${banner}<div class="flex gap-2">
      <a href="${profileHref}" data-ifeed-author="${escapeHtml(post.author)}" class="shrink-0">${avatar(post.authorName || post.author, focused)}</a>
      <div class="min-w-0 flex-1">
        <div class="flex items-baseline gap-2"><a href="${profileHref}" data-ifeed-author="${escapeHtml(post.author)}" class="min-w-0 truncate hover:underline"><strong class="text-sm text-foreground">${escapeHtml(post.authorName || post.author)}</strong> <span class="text-sm text-muted-foreground">@${escapeHtml(post.author)}</span></a><a href="${threadHref}" data-ifeed-thread="${escapeHtml(post.id)}" class="ml-auto hover:underline">${timeLabel(post.createdAt)}</a></div>
        ${replying}${content}
        <footer class="mt-1 flex items-center gap-1">${actions}</footer>
      </div></div></article>`;
  }
  function entryCard(entry, options = {}) {
    if (entry.type === "boost") return postCard(entry.post, { ...options, boostedBy: { name: entry.booster, display: entry.boosterName || entry.booster } });
    return postCard(entry, options);
  }
  function rows(items, options = {}) {
    return items.map((item, index) => `<div${index ? ' class="border-t border-border"' : ""}>${entryCard(item, options(item) || {})}</div>`).join("");
  }
  function empty(title, desc) {
    return `<div class="fm-blankslate"><i data-lucide="messages-square"></i><h2 class="fm-blankslate-title">${escapeHtml(title)}</h2><p class="fm-blankslate-desc">${escapeHtml(desc)}</p></div>`;
  }

  function renderInstall() {
    $("[data-ifeed-install-panel]")?.classList.toggle("hidden", state.installed);
    $("[data-ifeed-app]")?.classList.toggle("hidden", !state.installed);
    $$("[data-ifeed-requires-install]").forEach((node) => node.classList.toggle("hidden", !state.installed));
  }
  function renderCompose() {
    const replying = $("[data-ifeed-replying]");
    replying?.classList.toggle("hidden", !state.replyTo);
    replying?.classList.toggle("flex", !!state.replyTo);
    const text = $("[data-ifeed-replying-text]");
    if (text) text.textContent = state.replyTo ? `Replying to @${state.replyTo.author}` : "";
    const length = ($("[data-ifeed-text]")?.value || "").length;
    const remaining = $("[data-ifeed-remaining]");
    if (remaining) { remaining.textContent = String(MAX_LENGTH - length); remaining.classList.toggle("text-destructive", length > MAX_LENGTH); }
  }
  function render() {
    renderInstall();
    if (!state.installed) return;
    const list = $("[data-ifeed-list]");
    const context = $("[data-ifeed-context]");
    const profile = $("[data-ifeed-profile]");
    const timeline = state.view.kind === "timeline";
    context?.classList.toggle("hidden", timeline);
    context?.classList.toggle("flex", !timeline);
    profile?.classList.toggle("hidden", state.view.kind !== "profile" || !state.profile);
    if (profile && state.view.kind === "profile" && state.profile) {
      const since = state.profile.since ? ` · posting since ${escapeHtml(new Date(state.profile.since).toLocaleDateString(undefined, { month: "long", year: "numeric" }))}` : "";
      profile.innerHTML = `<div class="flex items-center gap-3">${avatar(state.profile.displayName || state.profile.name, true)}<div class="min-w-0"><h2 class="truncate text-base font-semibold text-foreground">${escapeHtml(state.profile.displayName || state.profile.name)}</h2><p class="text-sm text-muted-foreground">@${escapeHtml(state.profile.name)} · ${escapeHtml(String(state.profile.posts))} ${state.profile.posts === 1 ? "post" : "posts"}${since}</p></div></div>`;
    }
    $("[data-ifeed-more]")?.classList.toggle("hidden", !state.next || state.view.kind === "thread");
    $("[data-ifeed-refresh]").disabled = state.loading;
    if (!list) return;
    if (state.loading && !state.posts.length && !state.thread) {
      list.innerHTML = '<div class="px-4 py-8 text-sm text-muted-foreground"><span class="fm-spinner"></span>Loading the instance feed…</div>';
    } else if (state.view.kind === "thread") {
      const thread = state.thread;
      const items = thread?.post ? [...thread.ancestors, thread.post, ...thread.replies] : [];
      list.innerHTML = items.length ? rows(items, (item) => ({ focused: item.id === thread.post.id })) : empty("Post not found", "It may have been deleted.");
    } else if (!state.posts.length) {
      list.innerHTML = state.view.kind === "profile"
        ? empty("No posts yet", `@${state.view.author} has not posted to the instance feed.`)
        : empty("No posts yet", "Be the first to post to this instance.");
    } else {
      list.innerHTML = rows(state.posts, () => ({}));
    }
    renderCompose();
    window.lucide?.createIcons?.();
  }

  function eachPost(callback) {
    state.posts.forEach((entry) => callback(entry.type === "boost" ? entry.post : entry));
    if (state.thread?.post) [...state.thread.ancestors, state.thread.post, ...state.thread.replies].forEach(callback);
  }
  function findPost(id) {
    let found = null;
    eachPost((post) => { if (post.id === id) found = post; });
    return found;
  }
  function replacePost(updated) {
    eachPost((post) => { if (post.id === updated.id) Object.assign(post, updated); });
  }

  async function load({ append = false } = {}) {
    state.loading = true;
    if (!append) state.next = "";
    render();
    try {
      if (state.view.kind === "thread") {
        state.thread = await request(`${API}/posts/${state.view.post}`).catch((error) => { if (error.status === 404) return null; throw error; });
        state.installed = true; state.posts = []; state.profile = null;
      } else {
        const params = new URLSearchParams();
        if (state.view.kind === "profile") params.set("author", state.view.author);
        if (append && state.next) params.set("before", state.next);
        const payload = await request(`${API}${params.toString() ? `?${params}` : ""}`);
        state.installed = payload.installed !== false;
        state.thread = null;
        state.profile = payload.profile || null;
        state.posts = append ? state.posts.concat(payload.posts || []) : (payload.posts || []);
        state.next = payload.next || "";
      }
      message("error", "");
    } catch (error) {
      if (error.status === 409) state.installed = false;
      else message("error", `Could not load the feed: ${error.message}`);
    } finally {
      state.loading = false;
      render();
    }
  }
  function go(view) {
    state.view = view; state.posts = []; state.thread = null; state.profile = null;
    history.pushState(null, "", urlFor(view));
    $("[data-view='instance-feed']")?.scrollTo?.({ top: 0 });
    void load();
  }

  async function submit(event) {
    event.preventDefault();
    const textarea = $("[data-ifeed-text]");
    const spoilerInput = $("[data-ifeed-spoiler]");
    const text = String(textarea?.value || "").trim();
    if (!text) return;
    if (text.length > MAX_LENGTH) return message("error", `Posts can be at most ${MAX_LENGTH} characters.`);
    const button = $("[data-ifeed-submit]"); if (button) button.disabled = true;
    try {
      const spoiler = spoilerInput && !spoilerInput.classList.contains("hidden") ? spoilerInput.value.trim() : "";
      const { post } = await request(`${API}/posts`, "POST", { text, spoiler, replyTo: state.replyTo?.id || "" });
      textarea.value = ""; if (spoilerInput) spoilerInput.value = "";
      const parent = state.replyTo; state.replyTo = null;
      message("error", "");
      if (parent && state.view.kind !== "thread") go({ kind: "thread", post: parent.id });
      else if (state.view.kind === "thread") await load();
      else if (state.view.kind === "timeline" || state.view.author === post.author) { state.posts.unshift(post); render(); }
      else render();
    } catch (error) {
      message("error", `Could not post: ${error.message}`);
    } finally {
      if (button) button.disabled = false;
    }
  }
  async function act(kind, id) {
    const post = findPost(id); if (!post) return;
    if (kind === "reply") {
      state.replyTo = { id: post.id, author: post.author };
      const textarea = $("[data-ifeed-text]");
      if (textarea && !textarea.value.startsWith(`@${post.author}`)) textarea.value = `@${post.author} ${textarea.value}`.trimEnd() + " ";
      renderCompose(); $("[data-ifeed-compose]")?.scrollIntoView({ block: "nearest" }); textarea?.focus();
      return;
    }
    try {
      if (kind === "delete") {
        if (!confirm("Delete this post? Its favourites and boosts are removed with it.")) return;
        await request(`${API}/posts/${id}`, "DELETE");
        if (state.view.kind === "thread" && state.thread?.post?.id === id) return go({ kind: "timeline" });
        return void load();
      }
      const active = kind === "boost" ? post.boosted : post.favourited;
      const { post: updated } = await request(`${API}/posts/${id}/${kind}`, active ? "DELETE" : "POST", {});
      replacePost(updated);
      if (kind === "boost" && state.view.kind === "timeline") return void load();
      render();
    } catch (error) {
      message("error", `Could not update the post: ${error.message}`);
    }
  }
  async function install() {
    const button = $("[data-ifeed-install]"); if (button) button.disabled = true;
    try {
      await request(`/api/plugins/@account/${PACKAGE}/install`, "POST", {});
      message("notice", "Instance Feed installed.");
      await load();
    } catch (error) {
      message("error", `Could not install: ${error.message}`);
    } finally {
      if (button) button.disabled = false;
    }
  }
  async function exportMine() {
    try { const snapshot = await request(`${API}/export`); downloadJson(snapshot.export || {}, snapshot.filename || "forkmesh-instance-feed.json"); }
    catch (error) { message("error", `Could not export: ${error.message}`); }
  }
  async function uninstall() {
    if (!confirm("Uninstall Instance Feed? Your posts, boosts and favourites download first, then they are deleted from the feed.")) return;
    try {
      const snapshot = await request(`${API}/export`);
      const data = snapshot.export || {};
      if ((data.posts || []).length || (data.boosts || []).length || (data.favourites || []).length) downloadJson(data, snapshot.filename || "forkmesh-instance-feed.json");
      await request(`/api/plugins/@account/${PACKAGE}/uninstall`, "POST", {});
      $("[data-ifeed-manage-dialog]")?.close();
      state.installed = false; state.posts = []; state.thread = null;
      message("notice", "Instance Feed uninstalled.");
      render();
    } catch (error) {
      message("error", `Could not uninstall: ${error.message}`);
    }
  }

  function init() {
    if (!session()?.sessionToken) return message("error", "Sign in to use the instance feed.");
    $("[data-ifeed-install]")?.addEventListener("click", () => void install());
    $("[data-ifeed-refresh]")?.addEventListener("click", () => void load());
    $("[data-ifeed-more]")?.addEventListener("click", () => void load({ append: true }));
    $("[data-ifeed-back]")?.addEventListener("click", () => go({ kind: "timeline" }));
    $("[data-ifeed-compose]")?.addEventListener("submit", (event) => void submit(event));
    $("[data-ifeed-text]")?.addEventListener("input", renderCompose);
    $("[data-ifeed-text]")?.addEventListener("keydown", (event) => {
      if (event.key === "Enter" && (event.metaKey || event.ctrlKey)) { event.preventDefault(); $("[data-ifeed-compose]")?.requestSubmit(); }
    });
    $("[data-ifeed-reply-cancel]")?.addEventListener("click", () => { state.replyTo = null; renderCompose(); });
    $("[data-ifeed-spoiler-toggle]")?.addEventListener("click", (event) => {
      const input = $("[data-ifeed-spoiler]"); if (!input) return;
      const show = input.classList.contains("hidden");
      input.classList.toggle("hidden", !show); event.currentTarget.setAttribute("aria-pressed", show ? "true" : "false");
      event.currentTarget.classList.toggle("text-accent", show);
      if (show) input.focus(); else input.value = "";
    });
    $("[data-ifeed-list]")?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-ifeed-action]");
      if (button) { event.preventDefault(); return void act(button.dataset.ifeedAction, button.dataset.ifeedId); }
      const reveal = event.target.closest("[data-ifeed-reveal]");
      if (reveal) {
        const id = reveal.dataset.ifeedReveal;
        if (state.revealed.has(id)) state.revealed.delete(id); else state.revealed.add(id);
        return render();
      }
      if (event.metaKey || event.ctrlKey || event.shiftKey || event.button) return;
      const author = event.target.closest("[data-ifeed-author]");
      if (author) { event.preventDefault(); return go({ kind: "profile", author: author.dataset.ifeedAuthor }); }
      const thread = event.target.closest("[data-ifeed-thread]");
      if (thread) { event.preventDefault(); return go({ kind: "thread", post: thread.dataset.ifeedThread }); }
      const article = event.target.closest("[data-ifeed-post]");
      if (article && !event.target.closest("a,button") && !getSelection()?.toString() && article.dataset.ifeedPost !== state.thread?.post?.id) {
        go({ kind: "thread", post: article.dataset.ifeedPost });
      }
    });
    $("[data-ifeed-manage]")?.addEventListener("click", () => $("[data-ifeed-manage-dialog]")?.showModal());
    $("[data-ifeed-export]")?.addEventListener("click", () => void exportMine());
    $("[data-ifeed-uninstall]")?.addEventListener("click", () => void uninstall());
    $$("[data-ifeed-dialog-close]").forEach((button) => button.addEventListener("click", () => button.closest("dialog")?.close()));
    window.addEventListener("popstate", () => { state.view = viewFromUrl(); state.posts = []; state.thread = null; void load(); });
    return load();
  }
  return init();
});

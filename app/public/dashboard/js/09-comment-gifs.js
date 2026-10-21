  // GIFs and stickers in comment composers (docs/app/comment-gifs.md).
  //
  // Every record composer (a textarea marked data-mention-input="record":
  // issue, pull request and discussion comments, replies, reviews, edits)
  // gets a Sticker button and, when the instance has a GIPHY key, a GIF
  // button. Both open one picker that inserts Markdown at the caret. GIF
  // searches go through /api/gifs/* so the key never reaches the browser,
  // and the picker shows "Powered by GIPHY" as GIPHY's terms ask. A GIF
  // reply (from the reaction picker) drops the GIF into the record's
  // composer and sends it. Rendered GIFs (mdGifHtml) can be paused, and show
  // a still frame under prefers-reduced-motion until hovered or pressed.
  const COMMENT_GIF_SEARCH_DELAY_MS = 350;
  const COMMENT_GIF_ATTRIBUTION = "Powered by GIPHY";
  const commentGifUi = {
    config: null,
    configLoad: null,
    picker: null,
    searchTimer: 0,
    observer: null,
    queued: false,
  };

  function commentGifsEnabled() {
    return commentGifUi.config?.enabled === true;
  }

  async function loadCommentGifConfig() {
    if (commentGifUi.config) return commentGifUi.config;
    if (commentGifUi.configLoad) return commentGifUi.configLoad;
    commentGifUi.configLoad = (async () => {
      try {
        const data = await fetchJson("/api/gifs/config");
        commentGifUi.config = { enabled: data?.enabled === true, rating: String(data?.rating || "pg") };
      } catch (_) {
        commentGifUi.config = { enabled: false, rating: "pg" };
      }
      document.querySelectorAll("[data-composer-gif]").forEach((button) => { button.hidden = !commentGifsEnabled(); });
      return commentGifUi.config;
    })();
    return commentGifUi.configLoad;
  }

  function commentComposerToolHtml(kind) {
    if (kind === "gif") {
      return `<button type="button" data-composer-gif hidden aria-label="Add a GIF" title="Add a GIF" aria-haspopup="dialog" class="fm-composer-media"><span class="fm-gif-chip" aria-hidden="true">GIF</span></button>`;
    }
    return `<button type="button" data-composer-sticker aria-label="Add a sticker" title="Add a sticker" aria-haspopup="dialog" class="fm-composer-media"><i data-lucide="sticker" class="h-4 w-4"></i></button>`;
  }

  // Adds the Sticker and GIF buttons to every record composer on the page
  // that does not have them yet: in the issue composer's toolbar, else in a
  // small row under the text box.
  function decorateCommentComposers() {
    commentGifUi.queued = false;
    if (!state.session?.sessionToken) return;
    let added = false;
    document.querySelectorAll('textarea[data-mention-input="record"]:not([data-composer-media])').forEach((textarea) => {
      textarea.setAttribute("data-composer-media", "");
      const tools = `${commentComposerToolHtml("sticker")}${commentComposerToolHtml("gif")}`;
      const toolbar = textarea.closest("form")?.querySelector("[data-repo-issue-comment-toolbar]");
      if (toolbar && !toolbar.querySelector("[data-composer-sticker]")) {
        toolbar.insertAdjacentHTML("beforeend", `<span aria-hidden="true" class="mx-1 h-4 w-px bg-border"></span>${tools}`);
      } else if (!toolbar) {
        const row = document.createElement("div");
        row.className = "fm-composer-media-row";
        row.innerHTML = tools;
        const after = textarea.parentElement?.tagName === "LABEL" ? textarea.parentElement : textarea;
        after.insertAdjacentElement("afterend", row);
      }
      added = true;
    });
    if (!added) return;
    document.querySelectorAll("[data-composer-gif]").forEach((button) => { button.hidden = !commentGifsEnabled(); });
    window.lucide?.createIcons();
    void loadCommentGifConfig();
  }

  function queueComposerDecoration() {
    if (commentGifUi.queued) return;
    commentGifUi.queued = true;
    requestAnimationFrame(decorateCommentComposers);
  }

  function composerTextareaFor(button) {
    const form = button.closest("form");
    const scoped = form?.querySelector('textarea[data-mention-input="record"]');
    if (scoped) return scoped;
    const row = button.closest(".fm-composer-media-row");
    const before = row?.previousElementSibling;
    return before?.matches?.("textarea") ? before : before?.querySelector?.("textarea") || null;
  }

  function insertIntoComposer(textarea, markdown) {
    if (!textarea) return;
    const { selectionStart: start = textarea.value.length, selectionEnd: end = textarea.value.length, value } = textarea;
    const before = value.slice(0, start);
    const lead = before && !before.endsWith("\n") ? "\n" : "";
    const text = `${lead}${markdown}\n`;
    textarea.value = `${before}${text}${value.slice(end)}`;
    const caret = start + text.length;
    textarea.focus();
    textarea.setSelectionRange(caret, caret);
    textarea.dispatchEvent(new Event("input", { bubbles: true }));
  }

  function gifMarkdown(gif) {
    const title = String(gif.title || "GIF").replace(/[[\]\n]/g, " ").trim() || "GIF";
    return `![${title}](${gif.url})`;
  }

  function stickerMarkdown(sticker) {
    const label = String(sticker.label || "Sticker").replace(/[[\]\n]/g, " ").trim() || "Sticker";
    return `![${label}](${sticker.src}#w=96)`;
  }

  function commentStickers() {
    const builtIn = Object.entries(COMMENT_REACTION_STICKERS).map(([slug, src]) => ({ id: slug, label: commentReactionLabel(slug), src }));
    const custom = Object.values(state.customReactions?.items || {}).map((item) => ({ id: item.emoji, label: item.label, src: item.src }));
    return builtIn.concat(custom);
  }

  function gifPreviewHtml(gif) {
    const motion = commentReactionMotionOk();
    const src = motion ? gif.preview : gif.still;
    return `<button type="button" data-gif-pick="${escapeHtml(gif.id)}" class="fm-gif-pick" aria-label="${escapeHtml(gif.title)}" title="${escapeHtml(gif.title)}"><img src="${escapeHtml(src)}" data-gif-preview="${escapeHtml(gif.preview)}" data-gif-still="${escapeHtml(gif.still)}" alt="" width="${Number(gif.width) || 200}" height="${Number(gif.height) || 200}" loading="lazy" decoding="async" referrerpolicy="no-referrer"></button>`;
  }

  function stickerPickHtml(sticker) {
    return `<button type="button" data-sticker-pick="${escapeHtml(sticker.id)}" class="fm-sticker-pick" aria-label="${escapeHtml(sticker.label)}" title="${escapeHtml(sticker.label)}"><img src="${escapeHtml(sticker.src)}" alt="" width="72" height="72" decoding="async"></button>`;
  }

  function renderGifPickerBody(picker) {
    const body = picker.el.querySelector("[data-gif-body]");
    if (!body) return;
    if (picker.tab === "stickers") {
      const stickers = commentStickers();
      picker.stickers = stickers;
      body.innerHTML = stickers.length
        ? `<div class="fm-sticker-grid" role="list">${stickers.map(stickerPickHtml).join("")}</div>`
        : '<p class="fm-gif-empty">No stickers here yet.</p>';
      return;
    }
    if (picker.loading && !picker.gifs.length) {
      body.innerHTML = '<p class="fm-gif-empty" role="status">Loading GIFs…</p>';
      return;
    }
    if (picker.error) {
      body.innerHTML = `<p class="fm-gif-empty" role="status">${escapeHtml(picker.error)}</p>`;
      return;
    }
    if (!picker.gifs.length) {
      body.innerHTML = `<p class="fm-gif-empty" role="status">${picker.query ? `No GIFs for "${escapeHtml(picker.query)}".` : "No GIFs right now."}</p>`;
      return;
    }
    body.innerHTML = `<div class="fm-gif-grid">${picker.gifs.map(gifPreviewHtml).join("")}</div>${picker.next != null ? '<button type="button" data-gif-more class="fm-btn fm-btn-sm fm-gif-more">More GIFs</button>' : ""}`;
  }

  async function loadGifPage(picker, append = false) {
    const query = picker.query;
    const offset = append ? picker.next || 0 : 0;
    const path = query
      ? `/api/gifs/search?q=${encodeURIComponent(query)}&offset=${offset}`
      : `/api/gifs/trending?offset=${offset}`;
    picker.loading = true;
    picker.error = "";
    if (!append) {
      picker.gifs = [];
      renderGifPickerBody(picker);
    }
    const ticket = (picker.ticket = (picker.ticket || 0) + 1);
    try {
      const data = await fetchJson(path);
      if (ticket !== picker.ticket || commentGifUi.picker !== picker) return;
      const gifs = (Array.isArray(data?.gifs) ? data.gifs : []).filter((gif) => gif && MD_GIPHY_GIF.test(String(gif.url || "")));
      picker.gifs = append ? picker.gifs.concat(gifs) : gifs;
      picker.next = Number.isInteger(data?.next) ? data.next : null;
    } catch (error) {
      if (ticket !== picker.ticket) return;
      picker.error = /429/.test(String(error?.message || "")) ? "Too many searches - wait a moment." : "GIFs are unavailable right now.";
    } finally {
      if (ticket === picker.ticket) picker.loading = false;
    }
    if (commentGifUi.picker === picker && picker.tab === "gifs") renderGifPickerBody(picker);
  }

  function closeGifPicker(restoreFocus = false) {
    const picker = commentGifUi.picker;
    if (!picker) return;
    commentGifUi.picker = null;
    clearTimeout(commentGifUi.searchTimer);
    picker.el.remove();
    picker.anchor?.setAttribute?.("aria-expanded", "false");
    if (restoreFocus && picker.anchor?.isConnected) picker.anchor.focus();
  }

  // One picker for GIFs and stickers. onPick receives {kind, markdown, gif}.
  function openGifPicker({ anchor, tab = "gifs", title = "Add to your comment", onPick }) {
    closeGifPicker();
    if (typeof closeCommentReactionPicker === "function") closeCommentReactionPicker();
    const gifs = commentGifsEnabled();
    const picker = { anchor, tab: gifs ? tab : "stickers", query: "", gifs: [], next: null, loading: false, error: "", onPick };
    const el = document.createElement("div");
    el.className = "fm-popover fm-gif-picker";
    el.setAttribute("role", "dialog");
    el.setAttribute("aria-label", title);
    const tabs = gifs
      ? `<div class="fm-segmented fm-gif-tabs" role="tablist" aria-label="Media"><button type="button" role="tab" data-gif-tab="gifs" aria-selected="${picker.tab === "gifs"}" aria-pressed="${picker.tab === "gifs"}">GIFs</button><button type="button" role="tab" data-gif-tab="stickers" aria-selected="${picker.tab === "stickers"}" aria-pressed="${picker.tab === "stickers"}">Stickers</button></div>`
      : '<div class="fm-gif-heading">Stickers</div>';
    el.innerHTML = `<div class="fm-gif-head">${tabs}<button type="button" data-gif-close class="fm-gif-close" aria-label="Close"><i data-lucide="x" class="h-4 w-4"></i></button></div>${gifs ? `<input type="search" data-gif-search class="fm-reaction-search" placeholder="Search GIPHY" aria-label="Search GIFs" autocomplete="off" spellcheck="false"${picker.tab === "gifs" ? "" : " hidden"}>` : ""}<div data-gif-body class="fm-gif-body" aria-live="polite"></div>${gifs ? `<div class="fm-gif-attribution" data-gif-attribution${picker.tab === "gifs" ? "" : " hidden"}>${COMMENT_GIF_ATTRIBUTION}</div>` : ""}`;
    document.body.append(el);
    commentGifUi.picker = Object.assign(picker, { el });
    anchor?.setAttribute?.("aria-expanded", "true");
    window.lucide?.createIcons();
    commentReactionPlace(el, anchor, "below");
    if (picker.tab === "gifs") void loadGifPage(picker);
    else renderGifPickerBody(picker);
    void loadCustomReactions().then((changed) => {
      if (changed && commentGifUi.picker === picker && picker.tab === "stickers") renderGifPickerBody(picker);
    });
    const search = el.querySelector("[data-gif-search]");
    if (search && picker.tab === "gifs" && !window.matchMedia?.("(max-width: 640px)").matches) search.focus();
    else el.querySelector("[data-gif-tab], [data-sticker-pick], [data-gif-close]")?.focus();
    search?.addEventListener("input", () => {
      clearTimeout(commentGifUi.searchTimer);
      commentGifUi.searchTimer = setTimeout(() => {
        picker.query = search.value.trim().slice(0, 50);
        void loadGifPage(picker);
      }, COMMENT_GIF_SEARCH_DELAY_MS);
    });
    el.addEventListener("click", (event) => {
      const tabButton = event.target.closest?.("[data-gif-tab]");
      if (tabButton) {
        picker.tab = tabButton.getAttribute("data-gif-tab");
        el.querySelectorAll("[data-gif-tab]").forEach((button) => {
          const on = button === tabButton;
          button.setAttribute("aria-selected", String(on));
          button.setAttribute("aria-pressed", String(on));
        });
        if (search) search.hidden = picker.tab !== "gifs";
        const credit = el.querySelector("[data-gif-attribution]");
        if (credit) credit.hidden = picker.tab !== "gifs";
        if (picker.tab === "gifs" && !picker.gifs.length) void loadGifPage(picker);
        else renderGifPickerBody(picker);
        return;
      }
      if (event.target.closest?.("[data-gif-close]")) {
        closeGifPicker(true);
        return;
      }
      if (event.target.closest?.("[data-gif-more]")) {
        void loadGifPage(picker, true);
        return;
      }
      const gifButton = event.target.closest?.("[data-gif-pick]");
      if (gifButton) {
        const gif = picker.gifs.find((item) => item.id === gifButton.getAttribute("data-gif-pick"));
        if (!gif) return;
        closeGifPicker(false);
        picker.onPick?.({ kind: "gif", markdown: gifMarkdown(gif), gif });
        return;
      }
      const stickerButton = event.target.closest?.("[data-sticker-pick]");
      if (stickerButton) {
        const sticker = (picker.stickers || []).find((item) => item.id === stickerButton.getAttribute("data-sticker-pick"));
        if (!sticker) return;
        closeGifPicker(false);
        picker.onPick?.({ kind: "sticker", markdown: stickerMarkdown(sticker), sticker });
      }
    });
    el.addEventListener("pointerover", (event) => gifPreviewMotion(event.target, true));
    el.addEventListener("pointerout", (event) => gifPreviewMotion(event.target, false));
    el.addEventListener("focusin", (event) => gifPreviewMotion(event.target, true));
    el.addEventListener("focusout", (event) => gifPreviewMotion(event.target, false));
    el.addEventListener("keydown", (event) => {
      if (event.key === "Escape") {
        event.preventDefault();
        closeGifPicker(true);
        return;
      }
      const cells = [...el.querySelectorAll("[data-gif-pick], [data-sticker-pick]")];
      const index = cells.indexOf(document.activeElement);
      if (index < 0 || !["ArrowRight", "ArrowLeft", "ArrowDown", "ArrowUp"].includes(event.key)) return;
      event.preventDefault();
      const columns = Math.max(1, Math.round(el.querySelector(".fm-gif-grid, .fm-sticker-grid")?.clientWidth / (cells[0]?.offsetWidth || 1)) || 1);
      const step = { ArrowRight: 1, ArrowLeft: -1, ArrowDown: columns, ArrowUp: -columns }[event.key];
      cells[Math.max(0, Math.min(cells.length - 1, index + step))]?.focus();
    });
  }

  // Previews are stills under reduced motion; hovering or focusing one plays
  // just that one.
  function gifPreviewMotion(target, on) {
    const image = target?.closest?.("[data-gif-pick]")?.querySelector("img");
    if (!image || commentReactionMotionOk()) return;
    image.src = on ? image.dataset.gifPreview : image.dataset.gifStill;
  }

  // The record's own composer and its send button, for a one-click GIF reply.
  function commentGifComposer() {
    const forms = [
      ["[data-repo-issue-comment-form]", "[data-repo-issue-comment-submit]"],
      ["[data-repo-discussion-reply-form]", "[data-repo-discussion-reply-submit]"],
      ["[data-repo-pull-review-form]", '[data-repo-pull-review-action="comment"]'],
    ];
    for (const [formSelector, submitSelector] of forms) {
      const form = document.querySelector(formSelector);
      const textarea = form?.querySelector('textarea[data-mention-input="record"]');
      if (form && textarea) return { form, textarea, submit: form.querySelector(submitSelector) };
    }
    return null;
  }

  function openGifReply(bar, anchor) {
    if (!state.session?.sessionToken) {
      showCommentReactionNotice(bar, "signin");
      return;
    }
    const subject = bar.getAttribute("data-comment-reactions") || "";
    const comment = bar.getAttribute("data-comment-id") || "";
    openGifPicker({
      anchor: anchor?.isConnected ? anchor : bar.querySelector("[data-reaction-add]"),
      title: "Reply with a GIF",
      onPick: ({ markdown }) => {
        const composer = commentGifComposer();
        if (!composer) {
          showCommentReactionNotice(bar, "Replies are closed here.");
          return;
        }
        const author = commentReactionAuthor(subject, comment).toLowerCase();
        const me = String(state.session?.nodeName || "").toLowerCase();
        const mention = author && author !== me && /^[a-z0-9][a-z0-9-]{0,62}$/.test(author) ? `@${author} ` : "";
        // A draft in progress is never replaced or sent: the GIF joins it.
        if (composer.textarea.value.trim() || !composer.submit) {
          insertIntoComposer(composer.textarea, `${mention}${markdown}`);
          composer.textarea.scrollIntoView({ block: "center", behavior: commentReactionMotionOk() ? "smooth" : "auto" });
          return;
        }
        composer.textarea.value = `${mention}${markdown}`;
        composer.textarea.dispatchEvent(new Event("input", { bubbles: true }));
        if (typeof composer.form.requestSubmit === "function") composer.form.requestSubmit(composer.submit);
        else composer.submit.click();
      },
    });
  }

  // Play/pause for rendered GIFs: the <source> carries the still frame for
  // reduced motion; "all" pins the still, "not all" pins the animation.
  function commentGifPlaying(holder) {
    const source = holder.querySelector("[data-fm-gif-still]");
    const media = source?.getAttribute("media") || "";
    if (media === "all") return false;
    if (media === "not all") return true;
    return commentReactionMotionOk();
  }

  function setCommentGifPlaying(holder, playing) {
    const source = holder.querySelector("[data-fm-gif-still]");
    if (!source) return;
    source.setAttribute("media", playing ? "not all" : "all");
    holder.setAttribute("aria-pressed", String(playing));
    holder.classList.toggle("fm-gif-paused", !playing);
  }

  function initCommentGifs() {
    document.addEventListener("click", (event) => {
      const sticker = event.target.closest?.("[data-composer-sticker]");
      const gif = event.target.closest?.("[data-composer-gif]");
      const button = sticker || gif;
      if (button) {
        event.preventDefault();
        const textarea = composerTextareaFor(button);
        if (commentGifUi.picker?.anchor === button) {
          closeGifPicker(true);
          return;
        }
        openGifPicker({
          anchor: button,
          tab: gif ? "gifs" : "stickers",
          onPick: ({ markdown }) => insertIntoComposer(textarea, markdown),
        });
        return;
      }
      const holder = event.target.closest?.("[data-fm-gif]");
      if (holder) {
        event.preventDefault();
        setCommentGifPlaying(holder, !commentGifPlaying(holder));
        return;
      }
      if (commentGifUi.picker && !commentGifUi.picker.el.contains(event.target)) closeGifPicker();
    });
    document.addEventListener("keydown", (event) => {
      const holder = event.target.closest?.("[data-fm-gif]");
      if (!holder || (event.key !== "Enter" && event.key !== " ")) return;
      event.preventDefault();
      setCommentGifPlaying(holder, !commentGifPlaying(holder));
    });
    // Under reduced motion a GIF plays only while it is hovered.
    document.addEventListener("pointerover", (event) => {
      const holder = event.target.closest?.("[data-fm-gif]");
      if (!holder || commentReactionMotionOk() || holder.contains(event.relatedTarget)) return;
      if (holder.querySelector("[data-fm-gif-still]")?.getAttribute("media") === "all") return;
      holder.querySelector("[data-fm-gif-still]")?.setAttribute("media", "not all");
    });
    document.addEventListener("pointerout", (event) => {
      const holder = event.target.closest?.("[data-fm-gif]");
      if (!holder || holder.contains(event.relatedTarget) || commentReactionMotionOk()) return;
      if (holder.getAttribute("aria-pressed") === "true") return;
      holder.querySelector("[data-fm-gif-still]")?.setAttribute("media", "(prefers-reduced-motion: reduce)");
    });
    window.addEventListener("resize", () => closeGifPicker());
    if (!state.session?.sessionToken) return;
    void loadCommentGifConfig();
    commentGifUi.observer = new MutationObserver(queueComposerDecoration);
    commentGifUi.observer.observe(document.body, { childList: true, subtree: true });
    queueComposerDecoration();
  }

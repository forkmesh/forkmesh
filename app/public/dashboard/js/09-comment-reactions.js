  const COMMENT_REACTION_LONG_PRESS_MS = 450;
  const COMMENT_REACTION_HOVER_MS = 300;
  const COMMENT_REACTION_NOTICE_MS = 2600;
  const COMMENT_REACTION_WHO_SHOWN = 8;
  const COMMENT_REACTION_STACK = 5;
  const COMMENT_REACTION_CONFETTI = 28;
  const COMMENT_REACTION_CONFETTI_COLORS = Object.freeze([
    "--dashboard-accent-rgb", "--dashboard-attention-rgb", "--dashboard-destructive-rgb",
    "--dashboard-success-rgb", "--dashboard-primary-rgb",
  ]);
  const commentReactionUi = {
    inflight: new Set(),
    confetti: new Set(),
    picker: null,
    who: null,
    whoChip: null,
    whoTimer: 0,
    noticeTimer: 0,
    press: null,
    suppressClick: null,
    loads: new Map(),
    customLoads: new Map(),
    walletLoads: new Map(),
  };

  function commentReactionMotionOk() {
    return !window.matchMedia?.("(prefers-reduced-motion: reduce)").matches;
  }

  function commentReactionBase(repo) {
    return repo ? repoApiBase(repo).toLowerCase() : "";
  }

  function commentReactionParts(subject) {
    const match = /^(issue|pull|discussion):([1-9]\d{0,9})$/.exec(String(subject || ""));
    return match ? { kind: match[1], number: Number(match[2]) } : null;
  }

  function seedCommentReactions() {
    const node = document.getElementById("forkmesh-comment-reactions");
    if (!node) return;
    let seed = null;
    try {
      seed = JSON.parse(node.textContent || "null");
    } catch (_) {
      seed = null;
    }
    if (!seed || !commentReactionParts(seed.subject) || !seed.reactions || typeof seed.reactions !== "object") return;
    state.commentReactions = {
      [seed.subject]: { base: String(seed.base || "").toLowerCase(), reactions: seed.reactions, seeded: true },
    };
  }

  function prepareCommentReactions(repo, kind, number) {
    const subject = commentReactionSubject(kind, number);
    const base = commentReactionBase(repo);
    const scope = `${base}|${subject}`;
    if (state.commentReactionScope === scope) return subject;
    state.commentReactionScope = scope;
    const kept = state.commentReactions?.[subject];
    state.commentReactions = kept && kept.base === base ? { [subject]: kept } : {};
    state.commentReactionLedger = {};
    return subject;
  }

  async function loadCommentReactions(repo, kind, number) {
    const subject = commentReactionSubject(kind, number);
    if (!repo || !subject) return null;
    const base = commentReactionBase(repo);
    const key = `${base}|${subject}`;
    if (commentReactionUi.loads.has(key)) return commentReactionUi.loads.get(key);
    const parts = commentReactionParts(subject);
    const signedIn = Boolean(state.session?.sessionToken);
    const load = (async () => {
      try {
        const data = await fetchJson(`${repoApiBase(repo)}/reactions?kind=${parts.kind}&number=${parts.number}`, { fresh: signedIn });
        if (!data || data.ok === false || !data.reactions || typeof data.reactions !== "object") return null;
        if (!state.commentReactions) state.commentReactions = {};
        state.commentReactions[subject] = { base, reactions: data.reactions, signedIn: data.signedIn === true };
        if (data.tips && typeof data.tips === "object") {
          if (!state.commentTips) state.commentTips = {};
          state.commentTips[subject] = data.tips;
        }
        refreshCommentReactionBars(subject);
        if (typeof loadCommentTipWallets === "function") void loadCommentTipWallets(subject);
        return data;
      } catch (_) {
        return null;
      } finally {
        commentReactionUi.loads.delete(key);
      }
    })();
    commentReactionUi.loads.set(key, load);
    return load;
  }

  function commentReactionLedgerFor(subject, comment) {
    return state.commentReactionLedger?.[`${subject}\u0000${comment}`] || null;
  }

  function redrawCommentReactionBar(bar) {
    const subject = bar.getAttribute("data-comment-reactions") || "";
    const comment = bar.getAttribute("data-comment-id") || "";
    const focused = bar.contains(document.activeElement)
      ? (document.activeElement.getAttribute("data-reaction") || (document.activeElement.hasAttribute("data-reaction-add") ? "+" : ""))
      : "";
    const expanded = commentReactionUi.picker?.bar === bar;
    const template = document.createElement("template");
    template.innerHTML = renderCommentReactionBar(subject, comment, commentReactionLedgerFor(subject, comment));
    const fresh = template.content.firstElementChild;
    if (!fresh) return;
    bar.replaceChildren(...fresh.childNodes);
    const add = bar.querySelector("[data-reaction-add]");
    if (expanded && add) {
      add.setAttribute("aria-expanded", "true");
      commentReactionUi.picker.button = add;
    }
    window.lucide?.createIcons();
    if (focused === "+") add?.focus();
    else if (focused) bar.querySelector(`[data-reaction="${CSS.escape(focused)}"]`)?.focus();
  }

  function refreshCommentReactionBars(subject) {
    document.querySelectorAll("[data-comment-reactions]").forEach((bar) => {
      if (bar.getAttribute("data-comment-reactions") === subject) redrawCommentReactionBar(bar);
    });
  }

  function commentReactionAuthor(subject, comment) {
    const parsed = state.repoRecordDetail?.parsed;
    if (!parsed) return "";
    if (comment === "body") {
      const values = parsed.values || {};
      return String(values.authorName || values.author || "");
    }
    const pools = [parsed.issueEvents, parsed.pullConversation, parsed.pullTimeline, parsed.discussionConversation];
    for (const pool of pools) {
      const event = (Array.isArray(pool) ? pool : []).find((ev) => ev && String(ev.id || "") === comment);
      if (event) return String(event.authorName || "");
    }
    return "";
  }

  function commentReactionApplyLocal(subject, comment, emoji, on) {
    const me = String(state.session?.nodeName || "");
    if (!state.commentReactions) state.commentReactions = {};
    const entry = state.commentReactions[subject]
      || (state.commentReactions[subject] = { base: commentReactionBase(state.repoRecordDetail?.repo), reactions: {} });
    const tallies = (Array.isArray(entry.reactions[comment]) ? entry.reactions[comment] : [])
      .map((tally) => ({ ...tally, people: Array.isArray(tally.people) ? [...tally.people] : [] }));
    let tally = tallies.find((item) => item.emoji === emoji);
    if (on) {
      if (!tally) {
        tally = { emoji, count: 0, mine: false, people: [] };
        const custom = state.customReactions?.items?.[emoji];
        if (custom) tally.custom = { label: custom.label, src: custom.src, animatedSrc: custom.animatedSrc || "" };
        tallies.push(tally);
      }
      if (!tally.mine) tally.count += 1;
      tally.mine = true;
      if (me && !tally.people.some((person) => String(person.name).toLowerCase() === me.toLowerCase())) {
        tally.people.unshift({ name: me, agent: false });
      }
    } else if (tally) {
      if (tally.mine) tally.count -= 1;
      tally.mine = false;
      tally.people = tally.people.filter((person) => String(person.name).toLowerCase() !== me.toLowerCase());
    }
    entry.reactions[comment] = tallies.filter((item) => item.count > 0);
  }

  function commentReactionErrorText(error) {
    const code = String(error?.message || "");
    if (code === "rate_limited") return "Slow down a little - try that reaction again in a moment.";
    if (code === "too_many_reactions") return "That's plenty of reactions on one comment.";
    if (code === "daily_limit") return "You've reached today's reaction limit.";
    if (code === "unauthorized") return "Sign in again to react.";
    if (code === "feature_disabled") return "Reactions are switched off here.";
    return "The reaction didn't save. Try again.";
  }

  async function toggleCommentReaction(bar, emoji) {
    const subject = bar.getAttribute("data-comment-reactions") || "";
    const comment = bar.getAttribute("data-comment-id") || "";
    const parts = commentReactionParts(subject);
    const repo = state.repoRecordDetail?.repo || state.selectedRepo;
    if (!parts || !comment || !repo || !commentReactionKnown(emoji)) return;
    if (!state.session?.sessionToken) {
      showCommentReactionNotice(bar, "signin");
      return;
    }
    const key = `${subject}|${comment}|${emoji}`;
    if (commentReactionUi.inflight.has(key)) return;
    const entry = state.commentReactions?.[subject];
    const before = (entry?.reactions?.[comment] || []).map((tally) => ({ ...tally }));
    const on = !before.some((tally) => tally.emoji === emoji && tally.mine);
    commentReactionUi.inflight.add(key);
    commentReactionApplyLocal(subject, comment, emoji, on);
    refreshCommentReactionBars(subject);
    if (on) celebrateCommentReaction(subject, comment, emoji);
    try {
      const response = await fetch(`${repoApiBase(repo)}/reactions`, {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({
          kind: parts.kind,
          number: parts.number,
          comment,
          emoji,
          on,
          author: commentReactionAuthor(subject, comment),
          sessionToken: state.session?.sessionToken || "",
        }),
      });
      const data = await response.json().catch(() => ({}));
      if (!response.ok || data.ok === false) throw new Error(String(data.error || `HTTP ${response.status}`));
      if (Array.isArray(data.reactions)) state.commentReactions[subject].reactions[comment] = data.reactions;
      refreshCommentReactionBars(subject);
    } catch (error) {
      if (state.commentReactions?.[subject]) state.commentReactions[subject].reactions[comment] = before;
      refreshCommentReactionBars(subject);
      const fresh = document.querySelector(`[data-comment-reactions="${CSS.escape(subject)}"][data-comment-id="${CSS.escape(comment)}"]`);
      if (fresh) showCommentReactionNotice(fresh, commentReactionErrorText(error));
    } finally {
      commentReactionUi.inflight.delete(key);
    }
  }

  function commentReactionChip(subject, comment, emoji) {
    const bar = document.querySelector(`[data-comment-reactions="${CSS.escape(subject)}"][data-comment-id="${CSS.escape(comment)}"]`);
    return bar?.querySelector(`[data-reaction="${CSS.escape(emoji)}"]`) || null;
  }

  function celebrateCommentReaction(subject, comment, emoji) {
    if (!commentReactionMotionOk()) return;
    const chip = commentReactionChip(subject, comment, emoji);
    if (!chip) return;
    chip.classList.remove("fm-reaction-pop");
    void chip.offsetWidth;
    chip.classList.add("fm-reaction-pop");
    chip.addEventListener("animationend", () => chip.classList.remove("fm-reaction-pop"), { once: true });
    const burst = document.createElement("span");
    burst.className = "fm-reaction-burst";
    burst.setAttribute("aria-hidden", "true");
    for (let index = 0; index < 6; index += 1) {
      const angle = (Math.PI * 2 * index) / 6 - Math.PI / 2;
      const particle = document.createElement("span");
      const image = chip.querySelector(".fm-reaction-img");
      if (image) particle.append(Object.assign(image.cloneNode(false), { width: 14, height: 14 }));
      else particle.textContent = ISSUE_REACTION_EMOJI[emoji] || "";
      particle.style.setProperty("--fm-dx", `${Math.round(Math.cos(angle) * 26)}px`);
      particle.style.setProperty("--fm-dy", `${Math.round(Math.sin(angle) * 26)}px`);
      particle.style.animationDelay = `${index * 12}ms`;
      burst.append(particle);
    }
    chip.append(burst);
    setTimeout(() => burst.remove(), 800);
    const once = `${subject}|${comment}`;
    if (emoji !== "celebrate" || commentReactionUi.confetti.has(once)) return;
    commentReactionUi.confetti.add(once);
    commentReactionConfetti(chip.getBoundingClientRect());
  }

  function commentReactionConfetti(rect) {
    if (typeof Element.prototype.animate !== "function") return;
    const originX = rect.left + rect.width / 2;
    const originY = rect.top + rect.height / 2;
    for (let index = 0; index < COMMENT_REACTION_CONFETTI; index += 1) {
      const piece = document.createElement("span");
      piece.className = "fm-reaction-confetti";
      piece.setAttribute("aria-hidden", "true");
      const color = COMMENT_REACTION_CONFETTI_COLORS[index % COMMENT_REACTION_CONFETTI_COLORS.length];
      piece.style.background = `rgb(var(${color}))`;
      piece.style.left = `${originX}px`;
      piece.style.top = `${originY}px`;
      document.body.append(piece);
      const angle = -Math.PI / 2 + (Math.random() - 0.5) * Math.PI * 1.1;
      const speed = 70 + Math.random() * 110;
      const dx = Math.cos(angle) * speed;
      const dy = Math.sin(angle) * speed;
      const spin = (Math.random() - 0.5) * 900;
      piece.animate([
        { transform: "translate(-50%, -50%) rotate(0deg)", opacity: 1 },
        { transform: `translate(calc(-50% + ${dx}px), calc(-50% + ${dy}px)) rotate(${spin / 2}deg)`, opacity: 1, offset: 0.45 },
        { transform: `translate(calc(-50% + ${dx * 1.3}px), calc(-50% + ${dy + 160}px)) rotate(${spin}deg)`, opacity: 0 },
      ], { duration: 1100 + Math.random() * 500, easing: "cubic-bezier(.2,.7,.4,1)", fill: "forwards" })
        .finished.catch(() => {}).then(() => piece.remove());
    }
  }

  function commentReactionPlace(el, anchor, prefer = "below") {
    const rect = anchor.getBoundingClientRect();
    const box = el.getBoundingClientRect();
    const margin = 8;
    let top = prefer === "above" ? rect.top - box.height - 6 : rect.bottom + 6;
    if (prefer === "above" && top < margin) top = rect.bottom + 6;
    if (prefer !== "above" && top + box.height > window.innerHeight - margin) top = Math.max(margin, rect.top - box.height - 6);
    const left = Math.min(Math.max(margin, rect.left + rect.width / 2 - box.width / 2), window.innerWidth - box.width - margin);
    el.style.top = `${Math.round(top)}px`;
    el.style.left = `${Math.round(Math.max(margin, left))}px`;
  }

  function commentReactionWhoElement() {
    if (commentReactionUi.who?.isConnected) return commentReactionUi.who;
    const el = document.createElement("div");
    el.id = "fm-reaction-who";
    el.className = "fm-popover fm-reaction-who";
    el.setAttribute("role", "tooltip");
    el.hidden = true;
    document.body.append(el);
    commentReactionUi.who = el;
    return el;
  }

  function renderCommentReactionWho(tally, signedIn) {
    const stack = tally.people.slice(0, COMMENT_REACTION_STACK)
      .map((person) => issueActorAvatarHtml(person.name, "", "h-5 w-5", person.agent, false)).join("");
    const rows = tally.people.slice(0, COMMENT_REACTION_WHO_SHOWN).map((person) => `<li>${issueActorAvatarHtml(person.name, "", "h-4 w-4", person.agent, false)}<span>${escapeHtml(person.name)}</span>${person.agent ? '<span class="fm-reaction-who-agent">agent</span>' : ""}</li>`).join("");
    const more = tally.count - Math.min(tally.people.length, COMMENT_REACTION_WHO_SHOWN);
    const verb = window.matchMedia?.("(pointer: coarse)").matches ? "Tap" : "Click";
    const hint = signedIn ? (tally.mine ? `${verb} to take yours back` : `${verb} to add yours`) : "Sign in to react";
    return `<div class="fm-reaction-who-head"><span class="fm-reaction-who-stack">${stack}</span><span aria-hidden="true" class="fm-reaction-who-glyph">${commentReactionGlyph(tally.emoji, tally.custom, 16)}</span><span>${escapeHtml(commentReactionLabel(tally.emoji, tally.custom))} · ${escapeHtml(formatCount(tally.count))}</span></div><ul>${rows}</ul>${more > 0 ? `<div class="fm-reaction-who-more">and ${escapeHtml(formatCount(more))} more</div>` : ""}<div class="fm-reaction-who-hint">${hint}</div>`;
  }

  function showCommentReactionWho(chip) {
    const bar = chip.closest("[data-comment-reactions]");
    if (!bar) return;
    const subject = bar.getAttribute("data-comment-reactions") || "";
    const comment = bar.getAttribute("data-comment-id") || "";
    const emoji = chip.getAttribute("data-reaction") || "";
    const tally = commentReactionTallies(subject, comment, commentReactionLedgerFor(subject, comment))
      .find((item) => item.emoji === emoji);
    if (!tally) return;
    clearTimeout(commentReactionUi.noticeTimer);
    const el = commentReactionWhoElement();
    el.style.pointerEvents = "";
    el.innerHTML = renderCommentReactionWho(tally, Boolean(state.session?.sessionToken));
    el.hidden = false;
    commentReactionUi.whoChip = chip;
    chip.setAttribute("aria-describedby", el.id);
    commentReactionPlace(el, chip, "above");
    window.lucide?.createIcons();
  }

  function hideCommentReactionWho() {
    clearTimeout(commentReactionUi.whoTimer);
    commentReactionUi.whoTimer = 0;
    commentReactionUi.whoChip?.removeAttribute("aria-describedby");
    commentReactionUi.whoChip = null;
    if (commentReactionUi.who) commentReactionUi.who.hidden = true;
  }

  function showCommentReactionNotice(bar, message) {
    hideCommentReactionWho();
    const el = commentReactionWhoElement();
    clearTimeout(commentReactionUi.noticeTimer);
    if (message === "signin") {
      const next = encodeURIComponent(location.pathname + location.search);
      el.innerHTML = `<div class="fm-reaction-who-head">Join the conversation</div><a href="/login?next=${next}" class="font-medium text-[var(--dashboard-link)] hover:underline">Sign in</a> to react.`;
      el.style.pointerEvents = "auto";
    } else {
      el.textContent = message;
      el.style.pointerEvents = "";
      if (typeof announceInterfaceStatus === "function") announceInterfaceStatus(message);
    }
    el.hidden = false;
    commentReactionPlace(el, bar.querySelector("[data-reaction-add]") || bar, "above");
    commentReactionUi.noticeTimer = setTimeout(() => {
      el.hidden = true;
      el.style.pointerEvents = "";
    }, message === "signin" ? COMMENT_REACTION_NOTICE_MS * 2 : COMMENT_REACTION_NOTICE_MS);
  }

  function commentReactionOptionHtml(slug, mine, custom = null) {
    const label = commentReactionLabel(slug, custom);
    return `<button type="button" role="gridcell" data-reaction-option="${escapeHtml(slug)}" aria-pressed="${mine.has(slug) ? "true" : "false"}" aria-label="${escapeHtml(label)}" title="${escapeHtml(label)}" class="fm-reaction-option">${commentReactionGlyph(slug, custom, 26)}</button>`;
  }

  function commentReactionSectionHtml(title, slugs, mine, customs = null) {
    if (!slugs.length) return "";
    const cells = slugs.map((slug) => commentReactionOptionHtml(slug, mine, customs?.[slug] || null)).join("");
    return `<div class="fm-reaction-section" role="presentation">${escapeHtml(title)}</div><div role="row" class="fm-reaction-grid">${cells}</div>`;
  }

  function renderCommentReactionPicker(bar, query = "") {
    const subject = bar.getAttribute("data-comment-reactions") || "";
    const comment = bar.getAttribute("data-comment-id") || "";
    const mine = new Set(commentReactionTallies(subject, comment, null)
      .filter((tally) => tally.mine).map((tally) => tally.emoji));
    const needle = String(query || "").trim().toLowerCase();
    const matches = COMMENT_REACTIONS.filter(([slug, label, words]) => !needle
      || `${slug} ${label} ${words}`.toLowerCase().includes(needle));
    const customs = state.customReactions?.items || {};
    const customMatches = Object.values(customs).filter((item) => !needle
      || `${item.slug} ${item.label}`.toLowerCase().includes(needle)).map((item) => item.emoji);
    if (needle) {
      if (!matches.length && !customMatches.length) return `<p class="fm-reaction-picker-empty">No reaction matches "${escapeHtml(query)}".</p>`;
      const found = matches.map(([slug]) => slug).concat(customMatches);
      return `<div role="row" class="fm-reaction-grid">${found.map((slug) => commentReactionOptionHtml(slug, mine, customs[slug] || null)).join("")}</div>`;
    }
    const quick = `<div role="row" class="fm-reaction-grid" data-reaction-quick>${COMMENT_REACTION_QUICK.map((slug) => commentReactionOptionHtml(slug, mine)).join("")}</div>`;
    const grouped = new Set([...COMMENT_REACTION_QUICK, ...COMMENT_REACTION_GRATITUDE]);
    const rest = matches.map(([slug]) => slug).filter((slug) => !grouped.has(slug));
    return `${quick}${commentReactionSectionHtml("Gratitude", [...COMMENT_REACTION_GRATITUDE], mine)}${commentReactionSectionHtml("This community", customMatches, mine, customs)}${commentReactionSectionHtml("More", rest, mine)}`;
  }

  function closeCommentReactionPicker(restoreFocus = false) {
    const picker = commentReactionUi.picker;
    if (!picker) return;
    commentReactionUi.picker = null;
    picker.el.remove();
    if (picker.button?.isConnected) {
      picker.button.setAttribute("aria-expanded", "false");
      if (restoreFocus) picker.button.focus();
    }
  }

  function openCommentReactionPicker(bar, button) {
    closeCommentReactionPicker();
    hideCommentReactionWho();
    if (!state.session?.sessionToken) {
      showCommentReactionNotice(bar, "signin");
      return;
    }
    const el = document.createElement("div");
    el.className = "fm-popover fm-reaction-picker";
    el.setAttribute("role", "dialog");
    el.setAttribute("aria-label", "Choose a reaction");
    const gifs = typeof commentGifsEnabled === "function" && commentGifsEnabled();
    el.innerHTML = `<input type="search" data-reaction-search class="fm-reaction-search" placeholder="Search reactions" aria-label="Search reactions" autocomplete="off" spellcheck="false"><div role="grid" aria-label="Reactions" data-reaction-options class="fm-reaction-options">${renderCommentReactionPicker(bar)}</div><div class="fm-reaction-picker-label" data-reaction-picker-label aria-live="polite"></div>${gifs ? `<button type="button" data-reaction-gif-reply class="fm-reaction-gif-reply"><span class="fm-gif-chip" aria-hidden="true">GIF</span>Reply with a GIF</button>` : ""}`;
    document.body.append(el);
    commentReactionUi.picker = { el, bar, button };
    button.setAttribute("aria-expanded", "true");
    commentReactionPlace(el, button, "below");
    void loadCustomReactions().then((changed) => {
      if (!changed || commentReactionUi.picker?.el !== el) return;
      const field = el.querySelector("[data-reaction-search]");
      el.querySelector("[data-reaction-options]").innerHTML = renderCommentReactionPicker(bar, field?.value || "");
    });
    const search = el.querySelector("[data-reaction-search]");
    const narrow = window.matchMedia?.("(max-width: 640px)").matches;
    if (narrow) el.querySelector("[data-reaction-option]")?.focus();
    else search?.focus();
    search?.addEventListener("input", () => {
      el.querySelector("[data-reaction-options]").innerHTML = renderCommentReactionPicker(bar, search.value);
    });
    el.addEventListener("pointerover", (event) => {
      const option = event.target.closest?.("[data-reaction-option]");
      const label = el.querySelector("[data-reaction-picker-label]");
      if (option && label) label.textContent = option.getAttribute("aria-label") || "";
    });
    el.addEventListener("focusin", (event) => {
      const option = event.target.closest?.("[data-reaction-option]");
      const label = el.querySelector("[data-reaction-picker-label]");
      if (option && label) label.textContent = option.getAttribute("aria-label") || "";
    });
    el.addEventListener("click", (event) => {
      if (event.target.closest?.("[data-reaction-gif-reply]")) {
        const target = commentReactionUi.picker?.bar || bar;
        closeCommentReactionPicker(false);
        if (typeof openGifReply === "function") openGifReply(target, button);
        return;
      }
      const option = event.target.closest?.("[data-reaction-option]");
      if (!option) return;
      const target = commentReactionUi.picker?.bar || bar;
      closeCommentReactionPicker(true);
      toggleCommentReaction(target, option.getAttribute("data-reaction-option") || "");
    });
    el.addEventListener("keydown", (event) => commentReactionPickerKey(event, el));
  }

  function commentReactionPickerKey(event, el) {
    if (event.key === "Escape") {
      event.preventDefault();
      closeCommentReactionPicker(true);
      return;
    }
    const options = [...el.querySelectorAll("[data-reaction-option]")];
    if (!options.length) return;
    const search = el.querySelector("[data-reaction-search]");
    const index = options.indexOf(document.activeElement);
    if (event.key === "Enter" && document.activeElement === search) {
      event.preventDefault();
      options[0].click();
      return;
    }
    const steps = { ArrowRight: 1, ArrowLeft: -1, ArrowDown: 6, ArrowUp: -6 };
    if (!(event.key in steps) && event.key !== "Home" && event.key !== "End") return;
    if (document.activeElement === search && (event.key === "ArrowLeft" || event.key === "ArrowRight")) return;
    event.preventDefault();
    let next;
    if (event.key === "Home") next = 0;
    else if (event.key === "End") next = options.length - 1;
    else if (index < 0) next = 0;
    else next = index + steps[event.key];
    if (next < 0 && document.activeElement !== search) {
      search?.focus();
      return;
    }
    options[Math.max(0, Math.min(options.length - 1, next))].focus();
  }

  async function loadCustomReactions() {
    const repo = state.repoRecordDetail?.repo || state.selectedRepo;
    const owner = String(repo?.owner || "").toLowerCase();
    if (state.customReactions?.owner === owner && state.customReactions.loaded) return false;
    if (commentReactionUi.customLoads.has(owner)) return commentReactionUi.customLoads.get(owner);
    const load = (async () => {
      try {
        const data = await fetchJson(`/api/reactions/custom${owner ? `?owner=${encodeURIComponent(owner)}` : ""}`);
        const items = {};
        for (const item of Array.isArray(data?.reactions) ? data.reactions : []) {
          if (!item || !COMMENT_REACTION_CUSTOM_RE.test(String(item.emoji || ""))) continue;
          const src = commentReactionSafeSrc(item.src);
          if (!src) continue;
          items[item.emoji] = { emoji: String(item.emoji), slug: String(item.slug || ""), label: String(item.label || item.slug || ""), src, animatedSrc: commentReactionSafeSrc(item.animatedSrc) };
        }
        state.customReactions = { owner, items, loaded: true };
        return Object.keys(items).length > 0;
      } catch (_) {
        state.customReactions = { owner, items: {}, loaded: true };
        return false;
      } finally {
        commentReactionUi.customLoads.delete(owner);
      }
    })();
    commentReactionUi.customLoads.set(owner, load);
    return load;
  }

  function sayCommentThanks(bar) {
    if (!bar) return;
    const subject = bar.getAttribute("data-comment-reactions") || "";
    const comment = bar.getAttribute("data-comment-id") || "";
    const mine = commentReactionTallies(subject, comment, commentReactionLedgerFor(subject, comment))
      .some((tally) => tally.emoji === "thankyou" && tally.mine);
    if (!mine) void toggleCommentReaction(bar, "thankyou");
  }

  function commentReactionAnimate(target, on) {
    const host = target?.closest?.("[data-reaction], [data-reaction-option]");
    const image = host?.querySelector?.("img[data-reaction-animated]");
    if (!image) return;
    if (on && commentReactionMotionOk()) {
      if (!image.dataset.reactionStill) image.dataset.reactionStill = image.getAttribute("src") || "";
      image.setAttribute("src", image.dataset.reactionAnimated);
    } else if (image.dataset.reactionStill) {
      image.setAttribute("src", image.dataset.reactionStill);
    }
  }

  function commentReactionChipFrom(target) {
    const chip = target?.closest?.("[data-reaction]");
    return chip && chip.closest("[data-comment-reactions]") ? chip : null;
  }

  function commentAnchorId() {
    const match = /^#comment-(.+)$/.exec(String(location.hash || ""));
    if (!match) return "";
    try {
      return decodeURIComponent(match[1]);
    } catch (_) {
      return "";
    }
  }

  function revealAnchoredComment() {
    const id = commentAnchorId();
    if (!id) return;
    const started = Date.now();
    let observer = null;
    const attempt = () => {
      const marker = document.querySelector(`[data-comment-id="${CSS.escape(id)}"]`);
      if (!marker) {
        if (Date.now() - started > 20000) observer?.disconnect();
        return false;
      }
      observer?.disconnect();
      const target = marker.closest("[data-repo-issue-comment]") || marker.parentElement || marker;
      [0, 300, 900, 1800].forEach((delay) => setTimeout(() => {
        if (target.isConnected) target.scrollIntoView({ block: "center" });
      }, delay));
      target.style.outline = "2px solid rgb(var(--dashboard-accent-rgb))";
      target.style.outlineOffset = "2px";
      target.style.borderRadius = target.style.borderRadius || "8px";
      setTimeout(() => {
        target.style.outline = "";
        target.style.outlineOffset = "";
      }, 4000);
      return true;
    };
    if (attempt()) return;
    observer = new MutationObserver(() => { attempt(); });
    observer.observe(document.body, { childList: true, subtree: true });
    setTimeout(() => observer?.disconnect(), 20000);
  }

  function initCommentAnchor() {
    revealAnchoredComment();
    window.addEventListener("hashchange", revealAnchoredComment);
    window.addEventListener("popstate", revealAnchoredComment);
    document.addEventListener("click", (event) => {
      if (event.target?.closest?.('a[href*="#comment-"]')) setTimeout(revealAnchoredComment, 50);
    });
  }

  function initCommentReactions() {
    seedCommentReactions();
    document.addEventListener("click", (event) => {
      const thanks = event.target.closest?.("[data-comment-reactions] [data-reaction-thanks]");
      if (thanks) {
        event.preventDefault();
        sayCommentThanks(thanks.closest("[data-comment-reactions]"));
        return;
      }
      const tip = event.target.closest?.("[data-comment-reactions] [data-comment-tip]");
      if (tip) {
        event.preventDefault();
        if (typeof openCommentTipSheet === "function") openCommentTipSheet(tip.closest("[data-comment-reactions]"), tip);
        return;
      }
      const add = event.target.closest?.("[data-comment-reactions] [data-reaction-add]");
      if (add) {
        event.preventDefault();
        const bar = add.closest("[data-comment-reactions]");
        if (commentReactionUi.picker?.bar === bar) closeCommentReactionPicker(true);
        else openCommentReactionPicker(bar, add);
        return;
      }
      const chip = commentReactionChipFrom(event.target);
      if (chip) {
        event.preventDefault();
        if (commentReactionUi.suppressClick === chip) {
          commentReactionUi.suppressClick = null;
          return;
        }
        hideCommentReactionWho();
        toggleCommentReaction(chip.closest("[data-comment-reactions]"), chip.getAttribute("data-reaction") || "");
        return;
      }
      if (commentReactionUi.picker && !commentReactionUi.picker.el.contains(event.target)) closeCommentReactionPicker();
    });
    document.addEventListener("pointerover", (event) => {
      commentReactionAnimate(event.target, true);
      if (event.pointerType !== "mouse") return;
      const chip = commentReactionChipFrom(event.target);
      if (!chip || chip === commentReactionUi.whoChip) return;
      clearTimeout(commentReactionUi.whoTimer);
      commentReactionUi.whoTimer = setTimeout(() => showCommentReactionWho(chip), COMMENT_REACTION_HOVER_MS);
    });
    document.addEventListener("pointerout", (event) => {
      if (!event.target.closest?.("[data-reaction], [data-reaction-option]")?.contains(event.relatedTarget)) commentReactionAnimate(event.target, false);
      if (event.pointerType !== "mouse") return;
      const chip = commentReactionChipFrom(event.target);
      if (!chip || chip.contains(event.relatedTarget)) return;
      if (chip === document.activeElement && chip.matches(":focus-visible")) return;
      hideCommentReactionWho();
    });
    document.addEventListener("focusin", (event) => {
      const chip = commentReactionChipFrom(event.target);
      if (chip && chip.matches(":focus-visible")) showCommentReactionWho(chip);
    });
    document.addEventListener("focusout", (event) => {
      if (commentReactionChipFrom(event.target)) hideCommentReactionWho();
    });
    document.addEventListener("pointerdown", (event) => {
      if (event.pointerType === "mouse") return;
      const chip = commentReactionChipFrom(event.target);
      clearTimeout(commentReactionUi.press?.timer);
      if (!chip) {
        commentReactionUi.press = null;
        if (!commentReactionUi.who?.contains(event.target)) hideCommentReactionWho();
        return;
      }
      commentReactionUi.press = {
        chip,
        x: event.clientX,
        y: event.clientY,
        timer: setTimeout(() => {
          commentReactionUi.suppressClick = chip;
          showCommentReactionWho(chip);
          navigator.vibrate?.(8);
        }, COMMENT_REACTION_LONG_PRESS_MS),
      };
    });
    const cancelPress = (event) => {
      const press = commentReactionUi.press;
      if (!press) return;
      if (event.type === "pointermove" && Math.hypot(event.clientX - press.x, event.clientY - press.y) < 10) return;
      clearTimeout(press.timer);
      commentReactionUi.press = null;
    };
    document.addEventListener("pointermove", cancelPress, { passive: true });
    document.addEventListener("pointerup", cancelPress);
    document.addEventListener("pointercancel", cancelPress);
    document.addEventListener("contextmenu", (event) => {
      if (commentReactionChipFrom(event.target)) event.preventDefault();
    });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape" && commentReactionUi.who && !commentReactionUi.who.hidden) hideCommentReactionWho();
    });
    window.addEventListener("resize", () => {
      closeCommentReactionPicker();
      hideCommentReactionWho();
    });
    window.addEventListener("scroll", () => {
      if (commentReactionUi.picker) commentReactionPlace(commentReactionUi.picker.el, commentReactionUi.picker.button, "below");
      hideCommentReactionWho();
    }, { passive: true });
  }

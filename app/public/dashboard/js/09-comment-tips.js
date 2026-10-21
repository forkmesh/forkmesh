  // Tips on comments, wallet to wallet over Solana Pay
  // (docs/app/comment-tips.md).
  //
  // An author with a Solana address on their profile (the public "solana"
  // payout field) gets a Tip button on their comments. The sheet asks
  // POST /api/repo/<o>/<r>/tips for a Solana Pay transfer request (a fresh
  // reference key per request), shows it as a QR code (ForkMeshQR, as on the
  // profile's tips card) and an "Open in wallet" link, then asks
  // /api/tips/verify every few seconds while it is open. The tipper's wallet
  // signs and sends; the forge never sees a key or holds funds.
  const COMMENT_TIP_POLL_MS = 6000;
  const COMMENT_TIP_POLL_LIMIT_MS = 3 * 60 * 1000;
  const COMMENT_TIP_PRESETS = Object.freeze({ sol: ["0.01", "0.05", "0.1"], usdc: ["1", "5", "10"], token: ["10", "50", "100"] });
  const COMMENT_TIP_AMOUNT_RE = /^\d{1,12}(?:\.\d{1,18})?$/;
  const commentTipUi = { config: null, configLoad: null, sheet: null, loads: new Map() };

  async function loadCommentTipConfig() {
    if (commentTipUi.config) return commentTipUi.config;
    if (commentTipUi.configLoad) return commentTipUi.configLoad;
    commentTipUi.configLoad = (async () => {
      try {
        const data = await fetchJson("/api/tips/config");
        commentTipUi.config = {
          enabled: data?.enabled === true,
          tokens: (Array.isArray(data?.tokens) ? data.tokens : []).filter((token) => token && token.id && token.symbol),
          disclaimer: String(data?.disclaimer || "Tips go directly wallet to wallet; the forge never holds funds."),
        };
      } catch (_) {
        commentTipUi.config = { enabled: false, tokens: [], disclaimer: "" };
      }
      return commentTipUi.config;
    })();
    return commentTipUi.configLoad;
  }

  // The comment's author when they can be tipped by this viewer: tips on,
  // signed in, someone else, with a public address on their profile.
  function commentTipAuthor(subject, comment) {
    if (!commentTipUi.config?.enabled || !state.session?.sessionToken) return "";
    const author = String(commentReactionAuthor(subject, comment) || "").toLowerCase();
    if (!author || author === String(state.session?.nodeName || "").toLowerCase()) return "";
    return state.commentTipWallets?.[author] ? author : "";
  }

  async function loadCommentTipWallets(subject) {
    if (!state.session?.sessionToken) return;
    const config = await loadCommentTipConfig();
    if (!config.enabled) return;
    const names = new Set();
    document.querySelectorAll("[data-comment-reactions]").forEach((bar) => {
      if (bar.getAttribute("data-comment-reactions") !== subject) return;
      const author = String(commentReactionAuthor(subject, bar.getAttribute("data-comment-id") || "") || "").toLowerCase();
      if (/^[a-z0-9][a-z0-9-]{0,62}$/.test(author) && !(author in (state.commentTipWallets || {}))) names.add(author);
    });
    if (!names.size) {
      refreshCommentReactionBars(subject);
      return;
    }
    const list = [...names].sort().slice(0, 30);
    const key = list.join(",");
    if (commentTipUi.loads.has(key)) return commentTipUi.loads.get(key);
    const load = (async () => {
      try {
        const data = await fetchJson(`/api/tips/recipients?names=${encodeURIComponent(key)}`);
        if (!state.commentTipWallets) state.commentTipWallets = {};
        for (const name of list) state.commentTipWallets[name] = String(data?.wallets?.[name] || "");
        refreshCommentReactionBars(subject);
      } catch (_) {
        // No Tip buttons is the safe failure.
      } finally {
        commentTipUi.loads.delete(key);
      }
    })();
    commentTipUi.loads.set(key, load);
    return load;
  }

  function commentTipSheetHtml(author, config) {
    const tokens = config.tokens.map((token, index) => `<button type="button" data-tip-token="${escapeHtml(token.id)}" aria-pressed="${index === 0}">${escapeHtml(token.symbol)}</button>`).join("");
    return `<div class="fm-tip-head"><h2 id="fm-tip-title" class="fm-tip-title"><span aria-hidden="true">◎</span> Tip @${escapeHtml(author)}</h2><button type="button" data-tip-close class="fm-gif-close" aria-label="Close"><i data-lucide="x" class="h-4 w-4"></i></button></div>
      <form data-tip-form class="fm-tip-form">
        <div class="fm-segmented fm-tip-tokens" role="group" aria-label="Token">${tokens}</div>
        <div class="fm-tip-presets" data-tip-presets role="group" aria-label="Amount presets"></div>
        <label class="fm-tip-amount">Amount<span class="fm-tip-amount-row"><input data-tip-amount type="text" inputmode="decimal" autocomplete="off" required aria-describedby="fm-tip-note"><span data-tip-symbol class="fm-tip-symbol"></span></span></label>
        <button type="submit" data-tip-create class="fm-btn fm-btn-cta fm-tip-create">Create payment request</button>
      </form>
      <div data-tip-request class="fm-tip-request" hidden>
        <div data-tip-qr class="fm-tip-qr" aria-label="Solana Pay QR code" role="img"></div>
        <div class="fm-tip-request-side">
          <p class="fm-tip-sum" data-tip-sum></p>
          <a data-tip-open class="fm-btn fm-btn-cta fm-tip-open" href="#" rel="noopener">Open in wallet</a>
          <button type="button" data-tip-copy class="fm-btn fm-btn-sm">Copy payment link</button>
          <button type="button" data-tip-check class="fm-btn fm-btn-sm">I've sent it - check now</button>
          <p class="fm-tip-address">To <code data-tip-address></code></p>
        </div>
      </div>
      <p data-tip-status class="fm-tip-status" role="status" aria-live="polite"></p>
      <p id="fm-tip-note" class="fm-tip-note">${escapeHtml(config.disclaimer)} Scan with a Solana Pay wallet (Phantom, Solflare, Backpack…) or open the link on this device.</p>`;
  }

  function commentTipToken(sheet) {
    const id = sheet.el.querySelector("[data-tip-token][aria-pressed='true']")?.getAttribute("data-tip-token") || "sol";
    return commentTipUi.config.tokens.find((token) => token.id === id) || commentTipUi.config.tokens[0];
  }

  function renderCommentTipPresets(sheet) {
    const token = commentTipToken(sheet);
    const presets = COMMENT_TIP_PRESETS[token.id] || COMMENT_TIP_PRESETS.token;
    const amount = sheet.el.querySelector("[data-tip-amount]");
    sheet.el.querySelector("[data-tip-presets]").innerHTML = presets.map((value) => `<button type="button" data-tip-preset="${value}" class="fm-tip-preset" aria-pressed="${amount.value === value}">${value} ${escapeHtml(token.symbol)}</button>`).join("");
    sheet.el.querySelector("[data-tip-symbol]").textContent = token.symbol;
  }

  function closeCommentTipSheet(restoreFocus = true) {
    const sheet = commentTipUi.sheet;
    if (!sheet) return;
    commentTipUi.sheet = null;
    clearTimeout(sheet.poll);
    sheet.backdrop.remove();
    if (restoreFocus && sheet.anchor?.isConnected) sheet.anchor.focus();
  }

  async function openCommentTipSheet(bar, anchor) {
    const subject = bar.getAttribute("data-comment-reactions") || "";
    const comment = bar.getAttribute("data-comment-id") || "";
    const author = commentTipAuthor(subject, comment);
    const config = await loadCommentTipConfig();
    if (!author || !config.enabled || !config.tokens.length) return;
    closeCommentTipSheet(false);
    if (typeof closeCommentReactionPicker === "function") closeCommentReactionPicker();
    const backdrop = document.createElement("div");
    backdrop.className = "fm-tip-backdrop";
    const el = document.createElement("div");
    el.className = "fm-popover fm-tip-sheet";
    el.setAttribute("role", "dialog");
    el.setAttribute("aria-modal", "true");
    el.setAttribute("aria-labelledby", "fm-tip-title");
    el.innerHTML = commentTipSheetHtml(author, config);
    backdrop.append(el);
    document.body.append(backdrop);
    const sheet = { el, backdrop, anchor, subject, comment, author, reference: "", poll: 0, startedAt: 0 };
    commentTipUi.sheet = sheet;
    const amount = el.querySelector("[data-tip-amount]");
    amount.value = (COMMENT_TIP_PRESETS[config.tokens[0].id] || COMMENT_TIP_PRESETS.token)[1];
    renderCommentTipPresets(sheet);
    window.lucide?.createIcons();
    amount.focus();
    amount.select();
    backdrop.addEventListener("click", (event) => {
      if (event.target === backdrop) closeCommentTipSheet();
    });
    el.addEventListener("keydown", (event) => {
      if (event.key === "Escape") {
        event.preventDefault();
        closeCommentTipSheet();
        return;
      }
      if (event.key !== "Tab") return;
      const focusable = [...el.querySelectorAll("button:not([hidden]), a[href]:not([hidden]), input")].filter((node) => node.offsetParent !== null);
      if (!focusable.length) return;
      const first = focusable[0];
      const last = focusable[focusable.length - 1];
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    });
    el.addEventListener("click", (event) => {
      if (event.target.closest("[data-tip-close]")) {
        closeCommentTipSheet();
        return;
      }
      const token = event.target.closest("[data-tip-token]");
      if (token) {
        el.querySelectorAll("[data-tip-token]").forEach((button) => button.setAttribute("aria-pressed", String(button === token)));
        const presets = COMMENT_TIP_PRESETS[token.getAttribute("data-tip-token")] || COMMENT_TIP_PRESETS.token;
        amount.value = presets[1];
        renderCommentTipPresets(sheet);
        return;
      }
      const preset = event.target.closest("[data-tip-preset]");
      if (preset) {
        amount.value = preset.getAttribute("data-tip-preset");
        renderCommentTipPresets(sheet);
        return;
      }
      if (event.target.closest("[data-tip-copy]")) {
        const link = el.querySelector("[data-tip-open]").getAttribute("href") || "";
        void copyTextToClipboard(link).then((copied) => {
          const button = el.querySelector("[data-tip-copy]");
          button.textContent = copied ? "Copied" : "Copy failed";
          setTimeout(() => { button.textContent = "Copy payment link"; }, 1500);
        });
        return;
      }
      if (event.target.closest("[data-tip-check]")) void checkCommentTip(sheet, true);
    });
    amount.addEventListener("input", () => renderCommentTipPresets(sheet));
    el.querySelector("[data-tip-form]").addEventListener("submit", (event) => {
      event.preventDefault();
      void createCommentTip(sheet);
    });
  }

  function setCommentTipStatus(sheet, text, tone = "") {
    const status = sheet.el.querySelector("[data-tip-status]");
    status.textContent = text;
    status.dataset.tone = tone;
  }

  async function createCommentTip(sheet) {
    const amount = sheet.el.querySelector("[data-tip-amount]").value.trim();
    const token = commentTipToken(sheet);
    if (!COMMENT_TIP_AMOUNT_RE.test(amount) || Number(amount) <= 0) {
      setCommentTipStatus(sheet, "Enter an amount, like 0.05.", "bad");
      return;
    }
    const parts = commentReactionParts(sheet.subject);
    const repo = state.repoRecordDetail?.repo || state.selectedRepo;
    if (!parts || !repo) return;
    const button = sheet.el.querySelector("[data-tip-create]");
    button.disabled = true;
    setCommentTipStatus(sheet, "Preparing the payment request…");
    try {
      const response = await fetch(`${repoApiBase(repo)}/tips`, {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({ kind: parts.kind, number: parts.number, comment: sheet.comment, recipient: sheet.author, token: token.id, amount, sessionToken: state.session?.sessionToken || "" }),
      });
      const data = await response.json().catch(() => ({}));
      if (!response.ok || data.ok === false || !/^solana:[1-9A-HJ-NP-Za-km-z]{32,44}\?/.test(String(data.uri || ""))) {
        throw new Error(String(data.error || `HTTP ${response.status}`));
      }
      if (commentTipUi.sheet !== sheet) return;
      sheet.reference = String(data.reference || "");
      sheet.startedAt = Date.now();
      sheet.el.querySelector("[data-tip-form]").hidden = true;
      const request = sheet.el.querySelector("[data-tip-request]");
      request.hidden = false;
      sheet.el.querySelector("[data-tip-open]").setAttribute("href", data.uri);
      sheet.el.querySelector("[data-tip-address]").textContent = String(data.address || "");
      sheet.el.querySelector("[data-tip-sum]").textContent = `${data.amount} ${data.symbol} to @${sheet.author}`;
      setCommentTipStatus(sheet, "Waiting for the transfer… this updates by itself.");
      const holder = sheet.el.querySelector("[data-tip-qr]");
      holder.dataset.tipQrValue = data.uri;
      void loadQrGenerator().then((loaded) => {
        if (holder.dataset.tipQrValue !== data.uri) return;
        try {
          if (loaded) holder.replaceChildren(window.ForkMeshQR.toCanvas(data.uri, 200));
          else holder.textContent = "QR unavailable";
        } catch (_) {
          holder.textContent = "QR unavailable";
        }
      });
      sheet.el.querySelector("[data-tip-open]").focus();
      sheet.poll = setTimeout(() => void checkCommentTip(sheet, false), COMMENT_TIP_POLL_MS);
    } catch (error) {
      const code = String(error?.message || "");
      const text = {
        no_wallet: `@${sheet.author} has no Solana address for tips any more.`,
        invalid_amount: "That amount is not allowed.",
        rate_limited: "Too many requests - try again in a moment.",
        daily_limit: "You've reached today's tip request limit.",
        self_tip: "You can't tip yourself.",
        unauthorized: "Sign in again to tip.",
      }[code] || "The payment request could not be created.";
      setCommentTipStatus(sheet, text, "bad");
    } finally {
      button.disabled = false;
    }
  }

  async function checkCommentTip(sheet, manual) {
    clearTimeout(sheet.poll);
    if (commentTipUi.sheet !== sheet || !sheet.reference) return;
    if (manual) setCommentTipStatus(sheet, "Checking the Solana network…");
    let status = "pending";
    try {
      const response = await fetch("/api/tips/verify", {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({ reference: sheet.reference, sessionToken: state.session?.sessionToken || "" }),
      });
      const data = await response.json().catch(() => ({}));
      status = response.ok ? String(data.status || "pending") : (response.status === 429 ? "busy" : "unavailable");
    } catch (_) {
      status = "unavailable";
    }
    if (commentTipUi.sheet !== sheet) return;
    if (status === "confirmed") {
      setCommentTipStatus(sheet, "Tip received - thank you! \u{1F4B8}", "ok");
      if (!state.commentTips) state.commentTips = {};
      const tips = state.commentTips[sheet.subject] || (state.commentTips[sheet.subject] = {});
      tips[sheet.comment] = (Number(tips[sheet.comment]) || 0) + 1;
      refreshCommentReactionBars(sheet.subject);
      sheet.el.querySelector("[data-tip-check]").hidden = true;
      return;
    }
    if (Date.now() - sheet.startedAt > COMMENT_TIP_POLL_LIMIT_MS) {
      setCommentTipStatus(sheet, "Not seen yet. If you sent it, it will show up within a few minutes.");
      return;
    }
    if (manual || status !== "pending") {
      setCommentTipStatus(sheet, status === "unavailable" ? "The Solana network did not answer - still waiting." : "Not seen yet - still waiting.");
    }
    sheet.poll = setTimeout(() => void checkCommentTip(sheet, false), COMMENT_TIP_POLL_MS);
  }

  function initCommentTips() {
    window.addEventListener("resize", () => {
      if (commentTipUi.sheet) commentTipUi.sheet.el.scrollTop = 0;
    });
    if (state.session?.sessionToken) void loadCommentTipConfig();
  }

(() => {
  const MENTION_RE = /(^|[^\w@/-])@([a-z][a-z0-9-]{1,30}[a-z0-9])(?![\w-])/i;
  let commandsPromise = null;
  let mentionable = [];

  function sessionToken() {
    try {
      const session = JSON.parse(localStorage.getItem("forkmesh.session") || "null");
      const token = String(session?.sessionToken || "");
      return token && token !== "cookie" ? token : "";
    } catch (_) {
      return "";
    }
  }

  async function request(method, path, body) {
    const headers = { accept: "application/json" };
    const token = sessionToken();
    if (token) headers.authorization = "Bearer " + token;
    const init = { method, headers, credentials: "same-origin", cache: "no-store" };
    if (body !== undefined) {
      headers["content-type"] = "application/json";
      init.body = JSON.stringify(body);
    }
    const response = await fetch(path, init);
    const payload = await response.json().catch(() => ({}));
    return { status: response.status, payload };
  }

  function commands() {
    if (!commandsPromise) {
      commandsPromise = request("GET", "/api/apps/commands")
        .then(({ payload }) => {
          mentionable = Array.isArray(payload.mentionable) ? payload.mentionable : [];
          return Array.isArray(payload.commands) ? payload.commands : [];
        })
        .catch(() => []);
    }
    return commandsPromise;
  }

  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = String(text);
    return node;
  }

  const TONES = {
    good: ["accepted", "delivered", "ok", "passed", "sent", "succeeded", "success", "done", "healthy", "up", "resolved"],
    warn: ["suppressed", "soft-bounced", "warning", "skipped", "queued", "retrying", "pending", "degraded"],
    bad: ["failed", "rejected", "bounced", "complained", "blocked", "error", "down", "critical"],
    info: ["opened", "clicked", "viewed", "noted"],
  };

  function toneOf(part) {
    const key = String(part || "").trim().toLowerCase().replace(/^[^a-z]+/, "");
    for (const [tone, words] of Object.entries(TONES)) {
      if (words.includes(key)) return tone;
    }
    return "";
  }

  function headlineNode(text) {
    const line = el("p", "chat-app-section chat-notice-headline");
    const parts = String(text).split(" \u00b7 ").map((part) => part.trim()).filter(Boolean);
    parts.forEach((part, index) => {
      const tone = index === 0 ? "" : toneOf(part);
      const className = index === 0
        ? "chat-notice-title"
        : "chat-notice-part" + (tone ? " is-" + tone : " is-muted");
      line.append(el("span", className, part));
    });
    return line;
  }

  function fieldNode(field) {
    const text = String(field);
    const match = text.match(/^((?:[^\w\s]+\s)?[A-Za-z][A-Za-z0-9 ]{0,23}): ([\s\S]+)$/);
    if (!match) return el("span", "chat-app-field", text);
    const row = el("span", "chat-app-field chat-notice-row");
    row.append(el("span", "chat-notice-label", match[1]), el("span", "chat-notice-value", match[2]));
    return row;
  }

  function renderBlocks(container, message, context) {
    const blocks = Array.isArray(message?.blocks) ? message.blocks : [];
    if (!blocks.length) return;
    const box = el("div", message?.notice ? "chat-app-blocks is-notice" : "chat-app-blocks");
    for (const block of blocks) {
      if (block?.type === "divider") {
        box.append(el("hr", "chat-app-divider"));
      } else if (block?.type === "section" || block?.type === "context") {
        const hasFields = Array.isArray(block.fields) && block.fields.length > 0;
        const card = Boolean(message?.notice) && hasFields && block.type === "section";
        const target = card ? el("div", "chat-notice-card") : box;
        if (block.text) {
          target.append(card
            ? headlineNode(block.text)
            : el("p", block.type === "context" ? "chat-app-context" : "chat-app-section", block.text));
        }
        if (hasFields) {
          const fields = el("div", "chat-app-fields");
          for (const field of block.fields) fields.append(card ? fieldNode(field) : el("span", "chat-app-field", field));
          target.append(fields);
        }
        if (target !== box) box.append(target);
      } else if (block?.type === "actions") {
        const row = el("div", "chat-app-actions");
        for (const element of block.elements || []) {
          if (element?.type === "button") {
            const button = el("button", "chat-app-button" + (element.style ? " chat-app-button-" + element.style : ""), element.text);
            button.type = "button";
            button.addEventListener("click", () => {
              void act(message, element.action_id, element.value || "", context, button);
            });
            row.append(button);
          } else if (element?.type === "static_select") {
            const select = el("select", "chat-app-select");
            const placeholder = el("option", "", element.placeholder || "Choose");
            placeholder.value = "";
            select.append(placeholder);
            for (const option of element.options || []) {
              const choice = el("option", "", option.text);
              choice.value = option.value;
              select.append(choice);
            }
            select.addEventListener("change", () => {
              if (select.value) void act(message, element.action_id, select.value, context, select);
            });
            row.append(select);
          }
        }
        box.append(row);
      }
    }
    container.append(box);
  }

  async function act(message, actionId, value, context, control) {
    if (!message?.appMessageId) return;
    if (control) control.disabled = true;
    try {
      const { payload } = await request("POST", "/api/apps/interact", {
        appMessageId: message.appMessageId,
        channelId: message.channelId || context?.channelId || "",
        actionId,
        value,
      });
      if (payload.responseType === "ephemeral") context?.showEphemeral?.(payload);
      else if (payload.ok === false) context?.showSystem?.(errorText(payload));
    } finally {
      if (control) control.disabled = false;
    }
  }

  function errorText(payload) {
    const name = payload?.app ? payload.app + " " : "The app ";
    return {
      unknown_command: "No installed app has that command. Type /apps to see yours.",
      app_unavailable: name + "did not answer.",
      rate_limited: "Too many requests to that app; try again in a minute.",
      message_expired: "That message is too old to use.",
    }[payload?.error] || "The app could not handle that (" + (payload?.error || "error") + ").";
  }

  async function run(text, context) {
    const source = String(text || "").trim();
    const match = source.match(/^\/([a-z][a-z0-9-]{1,31})(?:\s|$)/i);
    if (!match) return false;
    const name = match[1].toLowerCase();
    if (name === "apps") {
      const list = await commands();
      context?.showSystem?.(list.length
        ? "Your app commands: " + list.map((entry) => "/" + entry.command + " (" + entry.app.name + ")").join(", ")
        : "You have no app commands. Install apps from /dashboard/apps.");
      return true;
    }
    const list = await commands();
    if (!list.some((entry) => entry.command === name)) return false;
    const { payload } = await request("POST", "/api/apps/commands/run", {
      text: source,
      channelId: context?.channelId || "",
    });
    if (payload.responseType === "ephemeral") context?.showEphemeral?.(payload);
    else if (payload.ok === false) context?.showSystem?.(errorText(payload));
    return true;
  }

  function mentionsApp(text) {
    return MENTION_RE.test(String(text || ""));
  }

  async function forwardMentions(text, channelId, messageId) {
    if (!channelId || !mentionsApp(text)) return;
    await commands();
    const slugs = new Set(mentionable);
    const named = [...String(text).matchAll(new RegExp(MENTION_RE.source, "gi"))].map((found) => found[2].toLowerCase());
    if (!named.some((slug) => slugs.has(slug))) return;
    await request("POST", "/api/apps/mentions", { channelId, messageId, text }).catch(() => null);
  }

  function ephemeralNode(payload, context) {
    const row = el("div", "chat-app-ephemeral");
    const head = el("div", "chat-app-ephemeral-head");
    head.append(el("strong", "", payload?.app?.botName || "App"), el("span", "chat-app-ephemeral-note", " Only you can see this."));
    row.append(head);
    if (payload?.text) row.append(el("p", "chat-app-section", payload.text));
    renderBlocks(row, payload, context);
    return row;
  }

  window.ForkMeshChatApps = Object.freeze({ run, renderBlocks, forwardMentions, ephemeralNode, commands });
})();

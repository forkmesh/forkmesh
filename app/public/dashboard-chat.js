// Dashboard ForkMesh room chat integration.
// Keeps the current dashboard UI, but uses the same encrypted room protocol as
// the production chat from app/public/chat.js.
const chatModerationPromise = import("./chat-moderation.js").catch(() => null);
const chatSealPromise = import("./chat-seal.js").catch(() => null);
const chatFrameAuthPromise = import("./chat-frame-auth.js").catch(() => null);
const chatRetentionPromise = import("./chat-retention.js").catch(() => null);

function mountForkMeshDashboardChat(options = {}) {
  const ROOM_NAME = "general";
  const PUBLIC_WORLD_GENERAL_ROOM = "world-general";
  let roomPassphrase = null;
  const SPACE_CHANNELS = Object.freeze({
    "sky-campus": "#world-sky-campus",
    "space-station": "#world-space-station",
    "code-planet": "#world-code-planet",
    "organization-region": "#world-organization-region",
    "planet-atlas": "#world-planet-atlas",
    neighborhood: "#world-neighborhood",
    broadcast: "#world-broadcast",
  });
  const requestedParams = new URLSearchParams(location.search);
  const requestedSpace =
    requestedParams.get("space") || String(options?.space || "");
  const requestedOrganization = String(requestedParams.get("org") || "");
  const ROOM_OWNER = "mainnode";
  const ROOM_REPO = "forkmesh";
  let ACTIVE_SPACE = "";
  let PUBLIC_WORLD_GENERAL = true;
  let ACTIVE_ROOM = PUBLIC_WORLD_GENERAL_ROOM;
  let ROOM_KEY_ENDPOINT = "";
  let CHANNEL = "#general";
  let CHANNEL_LABEL = "General";
  let CHAT_WS_PATH = "";
  let SEAL_SCOPE = "public-world-general";
  function configureChatSpace(space) {
    ACTIVE_SPACE = Object.hasOwn(SPACE_CHANNELS, space) ? space : "";
    PUBLIC_WORLD_GENERAL = !ACTIVE_SPACE && !requestedOrganization;
    ACTIVE_ROOM = PUBLIC_WORLD_GENERAL ? PUBLIC_WORLD_GENERAL_ROOM : ROOM_NAME;
    ROOM_KEY_ENDPOINT =
      `/api/chat/room-key?owner=${encodeURIComponent(ROOM_OWNER)}` +
      `&repo=${encodeURIComponent(ROOM_REPO)}` +
      `&room=${encodeURIComponent(ACTIVE_ROOM)}`;
    CHANNEL = ACTIVE_SPACE ? SPACE_CHANNELS[ACTIVE_SPACE] : "#general";
    CHANNEL_LABEL = ACTIVE_SPACE
      ? ACTIVE_SPACE
          .split("-")
          .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
          .join(" ")
      : "General";
    CHAT_WS_PATH =
      `/api/repo/${encodeURIComponent(ROOM_OWNER)}` +
      `/${encodeURIComponent(ROOM_REPO)}/rooms/${encodeURIComponent(ACTIVE_ROOM)}/ws`;
    SEAL_SCOPE = PUBLIC_WORLD_GENERAL
      ? "public-world-general"
      : `${ROOM_OWNER}/${ROOM_REPO}/${ACTIVE_ROOM}`;
  }
  configureChatSpace(requestedSpace);
  const FORKBOT_ENDPOINT = "/api/forkbot/chat";
  // Cloudflare Workers AI model pick, shared with the public chat composer and
  // the dashboard home ForkBot composer (both write this key).
  const FORKBOT_MODEL_KEY = "forkmesh.forkbot.model";
  const FORKBOT_SENDER_ID = "forkbot";
  const FORKBOT_MENTION_RE = /(?:^|[^A-Za-z0-9_-])@?forkbot\b/i;
  const CLAUDE_SENDER_ID = "claude";
  const CODEX_SENDER_ID = "codex";

  function forkbotModel() {
    try {
      return localStorage.getItem(FORKBOT_MODEL_KEY) || "";
    } catch (_) {
      return "";
    }
  }

  async function moderateDashboardText(value) {
    const moderation = await chatModerationPromise;
    if (!moderation) return null;
    return moderation.moderateChatText(value);
  }

  async function moderateDashboardPlain(plain) {
    const moderation = await chatModerationPromise;
    if (!moderation) return null;
    return moderation.moderateChatPlain(plain);
  }
  // The task board addresses one general bot instead of naming a vendor.
  const ORG_BOT_SENDER_ID = "agent";
  // "codex"/"claude" survive only so a stored selection still routes to the
  // general bot after the per-vendor options were removed.
  const AGENT_ASSIGNEE_VALUES = ["agent", "codex", "claude"];
  const CLAUDE_MENTION_RE = /(?:^|[^A-Za-z0-9_-])@claude\b/i;
  const CODEX_MENTION_RE = /(?:^|[^A-Za-z0-9_-])@codex\b/i;
  // Mainnode base host for the room WebSocket. Defaults to the origin that
  // served the dashboard, so a self-hosted mainnode talks to itself. Override
  // with window.FORKMESH_RELAY_HOST to target a different relay (see
  // public docs, "Self-hosting and development" (/docs#self-hosting).
  const RELAY_HOST = window.FORKMESH_RELAY_HOST || location.host;
  const MAX_TEXT = 16000;
  const MAX_NAME = 32;
  const MAX_ATTACHMENT_BYTES = 1024 * 1024;
  const MAX_ATTACHMENT_NAME = 180;
  const MAX_ATTACHMENT_MIME = 100;
  const MAX_SIDE_MESSAGES = 3;
  const SLASH_COMMANDS = Object.freeze([
    {
      command: "/issue",
      usage: "/issue [owner/repo] <title>",
      description: "Create a repository issue",
    },
  ]);
  const REACTION_EMOJI = Object.freeze(["👍", "❤️", "😂", "🎉", "👀", "🚀"]);
  const STICKER_CATALOG = Object.freeze([
    Object.freeze({ slug: "lgtm", text: "LGTM", color: "#8250df", tilt: -6, emoji: "\u2705" }),
    Object.freeze({ slug: "ship-it", text: "Ship it", color: "#0969da", tilt: 5, emoji: "\u{1F680}" }),
    Object.freeze({ slug: "nice", text: "Nice!", color: "#bf3989", tilt: -4, emoji: "\u{1F44D}" }),
    Object.freeze({ slug: "thanks", text: "Thanks!", color: "#bc4c00", tilt: 6, emoji: "\u{1F64F}" }),
    Object.freeze({ slug: "on-it", text: "On it", color: "#57606a", tilt: -5, emoji: "\u{1F440}" }),
    Object.freeze({ slug: "wow", text: "Wow!", color: "#cf222e", tilt: 4, emoji: "\u{1F62E}" }),
    Object.freeze({ slug: "love-it", text: "Love it", color: "#d6336c", tilt: -3, emoji: "\u2764\uFE0F" }),
    Object.freeze({ slug: "party", text: "Party!", color: "#9a6700", tilt: 7, emoji: "\u{1F389}" }),
  ]);
  const STICKERS_BY_SLUG = new Map(
    STICKER_CATALOG.map((sticker) => [sticker.slug, sticker]),
  );
  const CHAT_MENTION_RE = /(^|[^A-Za-z0-9_-])@([a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)\b/gi;

  const fullLog = document.querySelector("#fullChatMessages");
  const sideLog = document.querySelector("#sideChatMessages");
  let fullLogPinPending = false;
  const LIVE_ROW_LIMIT = 150;
  const LIVE_ROW_RETIRE_BATCH = 30;
  const liveRowIds = [];
  let chatScrollThumbPending = false;
  let sideRenderPending = false;
  const fullInput = document.querySelector("#fullChatInput");
  const fullInputResizeHandle = document.querySelector(
    "[data-dashboard-chat-input-resize-handle]",
  );
  const sideInput = document.querySelector("#sideChatInput");
  const fullSend = document.querySelector("#fullChatSend");
  const fullTaskSend = document.querySelector("#fullChatTaskSend");
  const sideSend = document.querySelector("#sideChatSend");
  const fullChannel = document.querySelector("#fullChatChannel");
  const fullRepository = document.querySelector("#fullChatRepo");
  const fullAction = document.querySelector("#fullChatAction");
  const taskRouting = document.querySelector("[data-dashboard-task-routing]");
  const taskDepartment = document.querySelector("[data-dashboard-task-department]");
  const taskTeam = document.querySelector("[data-dashboard-task-team]");
  const taskDestination = document.querySelector("[data-dashboard-task-destination]");
  const taskAssignee = document.querySelector("[data-dashboard-task-assignee]");
  const fullSendLabel = document.querySelector(
    "[data-dashboard-chat-send-label]",
  );
  const fullComposerHint = document.querySelector(
    "[data-dashboard-chat-composer-hint]",
  );
  const fullComposerStatus = document.querySelector(
    "[data-dashboard-chat-composer-status]",
  );
  const contextChannel = document.querySelector(
    "[data-dashboard-chat-context-channel]",
  );
  const contextSource = document.querySelector(
    "[data-dashboard-chat-context-source]",
  );
  const contextAction = document.querySelector(
    "[data-dashboard-chat-context-action]",
  );
  const simpleWorldComposer = Boolean(
    document.querySelector("[data-world-simple-composer]"),
  );
  const chatScrollRail = document.querySelector(
    "[data-dashboard-chat-scroll-rail]",
  );
  const chatView = document.querySelector("[data-dashboard-chat-view]");
  const chatRail = document.querySelector("[data-dashboard-chat-rail]");
  const memberPane = document.querySelector("[data-dashboard-chat-members]");
  const CHAT_GROUP_WINDOW_MS = 7 * 60 * 1000;
  const MEMBER_LIST_LIMIT = 200;
  const MEMBER_AVATAR_FETCH_LIMIT = 24;
  const MEMBER_LIST_HIDDEN_KEY = "forkmesh.dashboard.chat.membersHidden";
  const MEMBER_STATS_ENDPOINT = "/api/chat/member-stats";
  const MEMBER_STATS_BATCH = 50;
  const MEMBER_STATS_TTL_MS = 5 * 60 * 1000;
  const MEMBER_STATS_RETRY_MS = 60 * 1000;
  const MEMBER_STATS_DELAY_MS = 120;
  const MEMBER_STATS_ICONS = Object.freeze({
    pulls: '<circle cx="18" cy="18" r="3"></circle><circle cx="6" cy="6" r="3"></circle><path d="M13 6h3a2 2 0 0 1 2 2v7"></path><path d="M6 9v12"></path>',
    commits: '<circle cx="12" cy="12" r="3"></circle><path d="M3 12h6"></path><path d="M15 12h6"></path>',
    issues: '<circle cx="12" cy="12" r="10"></circle><circle cx="12" cy="12" r="1"></circle>',
    reviews: '<path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"></path>',
    recent: '<circle cx="12" cy="12" r="10"></circle><path d="M12 6v6l4 2"></path>',
  });
  const MEMBER_STATS_LABELS = Object.freeze({
    pulls: ["pull request opened", "pull requests opened"],
    commits: ["commit", "commits"],
    issues: ["issue opened", "issues opened"],
    reviews: ["review", "reviews"],
  });
  const COMPOSER_EMOJI = Object.freeze([
    "😀", "😂", "😊", "😍", "🤔", "😎", "😢", "😮", "👍",
    "👎", "👏", "🙏", "🎉", "🔥", "🚀", "❤️", "👀", "✅",
  ]);
  let fullLogRegroupPending = false;
  let memberListPending = false;
  const memberStats = new Map();
  const memberStatsWanted = new Set();
  let memberStatsTimer = 0;
  let memberStatsInflight = false;
  let memberStatsObserver = null;
  let chatUserPanelName = "";
  let chatUserOnline = false;
  let composerEmojiPicker = null;

  if (!fullLog && !sideLog) return false;
  if (fullLog?.dataset.forkmeshChatMounted === "true") return true;
  if (fullLog) fullLog.dataset.forkmeshChatMounted = "true";

  let pendingWorldComposerPrefill = null;
  const seenParentNotifications = new Set();
  let fullInputManualHeight = 0;

  function defaultFullInputHeight() {
    if (!fullInput) return 48;
    const minHeight = Number.parseFloat(getComputedStyle(fullInput).minHeight);
    return Number.isFinite(minHeight) ? minHeight : 48;
  }

  function maxFullInputHeight() {
    return Math.max(
      defaultFullInputHeight(),
      Math.min(480, Math.round(window.innerHeight * 0.6)),
    );
  }

  function setFullInputManualHeight(height) {
    if (!fullInput) return;
    const minimum = defaultFullInputHeight();
    const nextHeight = Math.min(maxFullInputHeight(), Math.max(minimum, height));
    if (nextHeight <= minimum) {
      fullInputManualHeight = 0;
      fullInput.style.height = "";
    } else {
      fullInputManualHeight = nextHeight;
      fullInput.style.height = `${nextHeight}px`;
    }
    fullInputResizeHandle?.setAttribute(
      "aria-valuenow",
      String(Math.round(fullInputManualHeight || minimum)),
    );
  }

  function wireFullInputResizeHandle() {
    if (!fullInput || !fullInputResizeHandle) return;
    let drag = null;
    fullInputResizeHandle.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) return;
      event.preventDefault();
      drag = {
        pointerId: event.pointerId,
        startY: event.clientY,
        startHeight: fullInput.getBoundingClientRect().height,
      };
      fullInputResizeHandle.setPointerCapture(event.pointerId);
    });
    fullInputResizeHandle.addEventListener("pointermove", (event) => {
      if (!drag || event.pointerId !== drag.pointerId) return;
      setFullInputManualHeight(drag.startHeight + drag.startY - event.clientY);
    });
    const finishDrag = (event) => {
      if (!drag || event.pointerId !== drag.pointerId) return;
      if (fullInputResizeHandle.hasPointerCapture(event.pointerId)) {
        fullInputResizeHandle.releasePointerCapture(event.pointerId);
      }
      drag = null;
    };
    fullInputResizeHandle.addEventListener("pointerup", finishDrag);
    fullInputResizeHandle.addEventListener("pointercancel", finishDrag);
    fullInputResizeHandle.addEventListener("keydown", (event) => {
      if (event.key !== "ArrowUp" && event.key !== "ArrowDown") return;
      event.preventDefault();
      const currentHeight = fullInput.getBoundingClientRect().height;
      const step = event.shiftKey ? 48 : 16;
      setFullInputManualHeight(
        currentHeight + (event.key === "ArrowUp" ? step : -step),
      );
    });
  }

  function fileFromWorldComposerAttachment(value) {
    if (!value || typeof value !== "object") return null;
    const dataUrl = String(value.dataUrl || "");
    const match = dataUrl.match(
      /^data:(image\/(?:png|jpeg|webp));base64,([A-Za-z0-9+/=]+)$/,
    );
    if (!match || dataUrl.length > 700_000) return null;
    let bytes;
    try {
      const decoded = atob(match[2]);
      bytes = new Uint8Array(decoded.length);
      for (let index = 0; index < decoded.length; index += 1) {
        bytes[index] = decoded.charCodeAt(index);
      }
    } catch (_) {
      return null;
    }
    const suppliedName = safeAttachmentName(value.fileName || "world-screenshot.webp");
    return new File([bytes], suppliedName, {
      type: safeAttachmentMime(value.fileMime || match[1]),
    });
  }

  async function taskAttachmentMetadata(file) {
    const attachment = {
      name: safeAttachmentName(file?.name),
      mime: safeAttachmentMime(file?.type),
      size: Math.max(0, Number(file?.size) || 0),
    };
    if (
      !["image/png", "image/jpeg", "image/webp"].includes(attachment.mime) ||
      !file ||
      attachment.size > 1024 * 1024 ||
      typeof createImageBitmap !== "function"
    ) {
      return attachment;
    }
    try {
      const bitmap = await createImageBitmap(file);
      const side = 48;
      const canvas = document.createElement("canvas");
      canvas.width = side;
      canvas.height = side;
      const context = canvas.getContext("2d");
      const scale = Math.max(side / bitmap.width, side / bitmap.height);
      const width = bitmap.width * scale;
      const height = bitmap.height * scale;
      context.drawImage(
        bitmap,
        (side - width) / 2,
        (side - height) / 2,
        width,
        height,
      );
      bitmap.close?.();
      const thumbnail = canvas.toDataURL("image/webp", 0.72);
      if (thumbnail.length <= 10_000) attachment.thumbnail = thumbnail;
    } catch (_) {}
    return attachment;
  }

  function applyWorldComposerPrefill() {
    const data = pendingWorldComposerPrefill;
    if (!data) return false;
    const input = fullInput || sideInput;
    const control = attachmentControls.find(
      (candidate) => candidate.inputEl === input,
    );
    if (!input || (data.attachment && !control)) return false;
    const altText = String(data.attachment?.altText || "").slice(0, 500);
    input.value = String(data.text || altText || "");
    const file = fileFromWorldComposerAttachment(data.attachment);
    if (file && control) {
      stageDashboardAttachments(control, [file]);
      showAttachmentFeedback(
        control,
        "World screenshot attached. Choose a destination, then send.",
      );
    }
    pendingWorldComposerPrefill = null;
    input.focus();
    const caret = input.value.length;
    input.setSelectionRange?.(caret, caret);
    return true;
  }

  // The World's ForkBot avatar and CHAT bar ask this embed (parent frame,
  // same-origin) to drop starting text into the composer — e.g. "@forkbot "
  // so a visitor can start typing straight away. See world.js openChatTerminal.
  window.addEventListener("message", (event) => {
    if (event.origin !== location.origin) return;
    const data = event.data;
    if (!data) return;
    if (data.type === "forkmesh:chat-notification") {
      const id = String(data.id || "").slice(0, 96);
      if (id && seenParentNotifications.has(id)) return;
      const text = String(data.text || "").replace(/\s+/g, " ").trim().slice(
        0,
        240,
      );
      if (text) {
        if (id) seenParentNotifications.add(id);
        appendSystem(text);
      }
      return;
    }
    if (data.type !== "forkmesh:chat-prefill") return;
    pendingWorldComposerPrefill = {
      text: String(data.text || "").slice(0, MAX_TEXT),
      attachment:
        data.attachment && typeof data.attachment === "object"
          ? { ...data.attachment }
          : null,
    };
    applyWorldComposerPrefill();
  });

  function dispatchWorldChatPayload(payload) {
    if (document.querySelector("[data-world-native-chat]")) {
      window.dispatchEvent(
        new CustomEvent("forkmesh:world-chat-native", { detail: payload }),
      );
    } else if (window.parent !== window) {
      window.parent.postMessage(payload, location.origin);
    }
  }

  function dispatchWorldEmote(emote) {
    if (document.querySelector("[data-world-native-chat]")) {
      window.dispatchEvent(
        new CustomEvent("forkmesh:world-emote-native", {
          detail: { emote },
        }),
      );
    } else if (window.parent !== window) {
      window.parent.postMessage(
        { type: "forkmesh:world-emote", emote },
        location.origin,
      );
    }
  }

  dispatchWorldChatPayload({ type: "forkmesh:chat-ready" });

  const enc = new TextEncoder();
  const dec = new TextDecoder();
  let selfId = "";

  // Baseline behind the site-header chat badge, shared with chat.js and
  // site-header.js. Every retained #general line this browser contributes
  // advances it, so talking here never leaves an unread pill on the rest of
  // the site. No baseline yet means the header seeds one silently.
  const CHAT_ACTIVITY_SEEN_KEY = "forkmesh.chat.activitySeen";

  function noteOwnChatActivity() {
    try {
      const raw = localStorage.getItem(CHAT_ACTIVITY_SEEN_KEY);
      if (!raw) return;
      const seenActivity = JSON.parse(raw);
      if (!seenActivity || typeof seenActivity !== "object") return;
      localStorage.setItem(CHAT_ACTIVITY_SEEN_KEY, JSON.stringify({
        ...seenActivity,
        messageCount: (Number(seenActivity.messageCount) || 0) + 1,
        at: Date.now(),
      }));
    } catch (_) {}
  }

  let roomKey = null;
  let roomSeal = null;
  let sealModule = null;
  let sealStore = null;
  let frameAuth = null;
  let nodeOwners = null;
  let guestLines = null;
  const sealStores = new Map();
  let sealUnavailable = false;
  // Sender ids of frames not addressed to this browser. An encrypted room
  // would otherwise just look quiet.
  const unreadableSealKeyIds = new Set();
  let socket = null;
  let connecting = false;
  let openCallbacks = [];
  let cachedUserSession = null;
  // Agent conversations are not ordinary room traffic. The server separately
  // authorizes transcript visibility and queue admission; organization owners
  // may queue work for their linked desktop without gaining Engineering
  // transcript access.
  let orgAgentEngineeringAccess = false;
  let orgAgentQueueAccess = false;
  let orgAgentConversationAccess = false;
  let orgAgentAccessReason = "";
  let orgAgentAccessLoaded = false;
  const seen = new Set();
  const rows = new Map();
  // Shared `thread-reply` records keyed independently from top-level rows.
  // World mounts this same controller, so this store is the interoperable
  // thread model for both the dashboard and the in-world CHAT terminal.
  const threadRepliesById = new Map();
  const threadReplyIdsByRoot = new Map();
  let activeThreadRootId = "";
  let threadDialog = null;
  const HISTORY_INITIAL_MESSAGES = 5;
  const HISTORY_MIN_BATCH_MESSAGES = 5;
  const HISTORY_ESTIMATED_ROW_HEIGHT = 72;
  const HISTORY_PRELOAD_VIEWPORTS = 1.5;
  const historyRowIds = [];
  let historyVisibleCount = HISTORY_INITIAL_MESSAGES;
  let historyIndicator = null;
  let revealingHistory = false;
  let lastFullLogScrollTop = 0;
  let stickToNewest = true;
  let historyTouchStartY = null;
  let historyTouchRevealed = false;
  let historyWheelLatched = false;
  let historyWheelResetTimer = 0;
  let historyReplayTimer = 0;
  let historyReplayEnvelopes = [];
  let inboundFrameQueue = Promise.resolve();
  const DISCORD_REFRESH_MS = 60_000;
  const DISCORD_REFRESH_JITTER_MS = 15_000;
  const DISCORD_BACKOFF_BASE_MS = 60_000;
  const DISCORD_BACKOFF_MAX_MS = 15 * 60_000;
  const DISCORD_MAX_ORGANIZATIONS = 3;
  const DISCORD_MAX_CHANNELS = 5;
  const DISCORD_MAX_INITIAL_MESSAGES = 40;
  const DISCORD_APPEND_SLICE_MS = 6;
  let discordRefreshTimer = 0;
  let discordRefreshRunning = false;
  let discordInitialMessagesLoaded = false;
  let discordSources = null;
  let discordBackoffUntil = 0;
  let discordBackoffAttempts = 0;
  // messageId -> Map(emoji -> Map(reactorId -> reactorName)); identical to
  // the full web/Qt protocol shape so reactions converge across every client.
  const reactions = new Map();
  const stickers = new Map();
  const sideEntries = [];
  const attachmentControls = [];
  const attachmentUrls = new Set();
  // Rolling buffer of recent decrypted messages, forwarded to ForkBot so it can
  // resolve references like "that bug" from the conversation. The relay can
  // decrypt the default shared-key room; this controls only the narrower
  // context explicitly sent to ForkBot.
  const recentContext = [];
  const RECENT_CONTEXT_MAX = 20;
  function rememberContext(sender, text) {
    const clean = String(text || "").trim();
    if (!clean) return;
    recentContext.push({ sender: String(sender || "").slice(0, MAX_NAME), text: clean });
    if (recentContext.length > RECENT_CONTEXT_MAX) recentContext.shift();
  }
  const mentionProfileCache = new Map();
  const chatAvatarCache = new Map();
  let mentionCardEl = null;
  let activeMentionAnchor = null;
  let mentionHideTimer = null;
  // People seen in this room (live frames and replayed history), keyed by
  // lowercase handle: the relay keeps no roster, so this is what "who is here"
  // means for the composer's @mention list. mentionDirectory holds registered
  // accounts (fetched lazily on the first "@") so people who are offline right
  // now can still be mentioned.
  const mentionPeople = new Map();
  const mentionDirectory = new Map();
  let mentionDirectoryFetchedMs = 0;
  let mentionDirectoryPending = null;
  let mentionSuggestEl = null;
  // null when closed, else { input, items, index, start, end } where start/end
  // bound the "@partial" token in the input's value.
  let mentionSuggest = null;
  let slashSuggestEl = null;
  let slashSuggest = null;

  function bytesToB64(bytes) {
    const arr = new Uint8Array(bytes);
    let bin = "";
    for (let i = 0; i < arr.length; i += 1) bin += String.fromCharCode(arr[i]);
    return btoa(bin);
  }

  function b64ToBytes(value) {
    const bin = atob(value);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i += 1) out[i] = bin.charCodeAt(i);
    return out;
  }

  function safeAttachmentName(value) {
    const parts = String(value || "").replace(/\\/g, "/").split("/");
    const name = String(parts.pop() || "")
      .replace(/\0/g, "")
      .trim()
      .slice(0, MAX_ATTACHMENT_NAME);
    return name || "file";
  }

  function safeAttachmentMime(value) {
    const mime = String(value || "").trim().toLowerCase();
    return /^[a-z0-9][a-z0-9.+-]*\/[a-z0-9][a-z0-9.+-]*$/.test(mime) &&
      mime.length <= MAX_ATTACHMENT_MIME
      ? mime
      : "application/octet-stream";
  }

  function attachmentBlobType(value) {
    const mime = safeAttachmentMime(value);
    return /^(?:image\/(?:png|jpeg|gif|webp|avif|bmp)|video\/(?:mp4|webm|ogg|quicktime)|audio\/(?:mpeg|mp4|ogg|webm|wav|aac|flac)|text\/plain|application\/pdf)$/.test(mime)
      ? mime
      : "application/octet-stream";
  }

  function attachmentFromEntry(entry) {
    if (!entry || !entry.fileName || typeof entry.file !== "string") return null;
    if (!entry.file || entry.file.length > Math.ceil(MAX_ATTACHMENT_BYTES * 4 / 3) + 4) {
      return null;
    }
    try {
      const bytes = b64ToBytes(entry.file);
      if (!bytes.length || bytes.byteLength > MAX_ATTACHMENT_BYTES) return null;
      return {
        fileName: safeAttachmentName(entry.fileName),
        fileMime: safeAttachmentMime(entry.fileMime),
        file: entry.file,
        size: bytes.byteLength,
        objectUrl: "",
      };
    } catch (_) {
      return null;
    }
  }

  function attachmentObjectUrl(attachment) {
    if (attachment.objectUrl) return attachment.objectUrl;
    const bytes = b64ToBytes(attachment.file);
    const url = URL.createObjectURL(new Blob([bytes], { type: attachmentBlobType(attachment.fileMime) }));
    attachment.objectUrl = url;
    attachmentUrls.add(url);
    return url;
  }

  function releaseAttachment(attachment) {
    if (!attachment?.objectUrl) return;
    URL.revokeObjectURL(attachment.objectUrl);
    attachmentUrls.delete(attachment.objectUrl);
    attachment.objectUrl = "";
  }

  function formatAttachmentSize(size) {
    const bytes = Math.max(0, Number(size) || 0);
    if (bytes < 1024) return `${bytes} B`;
    return `${(bytes / 1024).toFixed(bytes < 10240 ? 1 : 0)} KiB`;
  }

  window.addEventListener("beforeunload", () => {
    for (const url of attachmentUrls) URL.revokeObjectURL(url);
    attachmentUrls.clear();
  });

  function b64urlToBytes(value) {
    let s = (value || "").replace(/-/g, "+").replace(/_/g, "/");
    while (s.length % 4) s += "=";
    return b64ToBytes(s);
  }

  // Filing a chat message as a repository issue lives in a module shared with
  // the full chat page (/chat-issue-filing.js) so the signed "open" event, the
  // author key, and the attribution wording cannot drift between surfaces. It
  // is imported on demand: the World loads this bundle alone, and neither the
  // dashboard nor the World needs issue signing until someone asks for it.
  let issueFilingPromise = null;

  function issueFiling() {
    if (!issueFilingPromise) {
      issueFilingPromise = import("/chat-issue-filing.js").catch((error) => {
        issueFilingPromise = null;
        throw error;
      });
    }
    return issueFilingPromise;
  }

  // The dashboard page carries its own new-issue signer (which also handles
  // agent assignment and offline pending issues), so prefer it when loaded.
  async function fileWebIssue(repository, title, body) {
    const shared = window.ForkMeshDashboardActions?.submitWebIssue;
    if (typeof shared === "function") return shared(repository, title, body);
    const filing = await issueFiling();
    return filing.fileWebIssue(
      repository, title, body, String(userSession()?.nodeName || ""));
  }

  async function ed25519Verify(pubB64url, sigB64url, dataStr) {
    try {
      const key = await crypto.subtle.importKey(
        "raw",
        b64urlToBytes(pubB64url),
        { name: "Ed25519" },
        false,
        ["verify"]
      );
      return await crypto.subtle.verify(
        { name: "Ed25519" },
        key,
        b64urlToBytes(sigB64url),
        enc.encode(dataStr)
      );
    } catch (_) {
      return false;
    }
  }

  async function fetchRoomPassphrase() {
    if (roomPassphrase) return roomPassphrase;
    const session = readSession();
    const token = session && session.sessionToken;
    const headers = { accept: "application/json" };
    if (token) headers.authorization = "Bearer " + token;
    const res = await fetch(ROOM_KEY_ENDPOINT, { headers, cache: "no-store" });
    if (!res.ok) {
      const err = new Error(
        PUBLIC_WORLD_GENERAL
          ? "Public World #general key unavailable."
          : "Sign in to join this authenticated chat — room key unavailable."
      );
      err.code = res.status === 401 || res.status === 403 ? "auth" : "server";
      throw err;
    }
    const data = await res.json().catch(() => ({}));
    if (!data || !data.passphrase) throw new Error("Room key unavailable.");
    roomPassphrase = String(data.passphrase);
    return roomPassphrase;
  }

  async function deriveRoomKey() {
    const passphrase = await fetchRoomPassphrase();
    const saltDigest = new Uint8Array(
      await crypto.subtle.digest(
        "SHA-256",
        enc.encode("ForkMesh room:" + ACTIVE_ROOM)
      )
    );
    const salt = saltDigest.slice(0, 16);
    const baseKey = await crypto.subtle.importKey(
      "raw",
      enc.encode(passphrase),
      "PBKDF2",
      false,
      ["deriveKey"]
    );
    return crypto.subtle.deriveKey(
      { name: "PBKDF2", salt, iterations: 210000, hash: "SHA-256" },
      baseKey,
      { name: "AES-GCM", length: 256 },
      false,
      ["encrypt", "decrypt"]
    );
  }

  function sessionAccount() {
    return String(userSession()?.nodeName || "").trim().toLowerCase();
  }

  function sealStoreForSession() {
    const account = sessionAccount();
    const profile = frameAuth.chatProfileFor(account);
    let store = sealStores.get(profile);
    if (!store) {
      store = sealModule.createRoomSealStore({ acct: account, profile });
      sealStores.set(profile, store);
    }
    return store;
  }

  async function activateChatIdentity() {
    const store = sealStoreForSession();
    selfId = await store.ready();
    sealStore = store;
    const account = sessionAccount();
    if (account) {
      const token = String(userSession()?.sessionToken || "");
      void frameAuth.bindNodeKey({
        store,
        account,
        owners: nodeOwners,
        headers: token && token !== "cookie" ? { authorization: `Bearer ${token}` } : {},
      }).catch(() => false);
    }
    return store;
  }

  async function loadRoomSeal() {
    sealModule = sealModule || (await chatSealPromise);
    frameAuth = frameAuth || (await chatFrameAuthPromise);
    if (!sealModule || !frameAuth) {
      roomSeal = null;
      sealUnavailable = !PUBLIC_WORLD_GENERAL;
      return null;
    }
    frameAuth.forgetLegacySelfId();
    nodeOwners = nodeOwners || frameAuth.createNodeOwners();
    guestLines = guestLines || frameAuth.createGuestRelabeler({
      owners: nodeOwners,
      onConfirmed: (node, account, ids) => renameConfirmedGuest(node, account, ids),
    });
    sealUnavailable = false;
    try {
      const store = await activateChatIdentity();
      roomSeal = await store.resolve(SEAL_SCOPE, ACTIVE_ROOM, RELAY_HOST);
    } catch (error) {
      roomSeal = null;
      sealUnavailable = !PUBLIC_WORLD_GENERAL;
      if (!PUBLIC_WORLD_GENERAL) throw error;
    }
    return roomSeal;
  }

  async function attributedPlains(plains) {
    if (!frameAuth || !nodeOwners) return [];
    const list = plains.filter(Boolean);
    await nodeOwners.prefetch(list, { selfNode: selfId });
    const attributed = list
      .map((plain) => frameAuth.attributeFrame(plain, {
        owners: nodeOwners,
        selfNode: selfId,
        selfAccount: sessionAccount(),
        selfName: displayName(),
      }))
      .filter(Boolean);
    for (const plain of attributed) guestLines?.note(plain);
    return attributed;
  }

  function renameConfirmedGuest(node, account, ids) {
    const name = String(account).slice(0, MAX_NAME);
    let side = false;
    let thread = false;
    for (const id of ids) {
      const record = rows.get(id) || threadRepliesById.get(id);
      if (record && record.senderId === node && !record.self) {
        record.who = name;
        record.accountKind = "user";
        if (record.rootId) thread = true;
        const author = record.el?.querySelector(".chat-message-author");
        if (author) {
          const holder = document.createElement("div");
          holder.innerHTML = authorElementHtml(record, author.className);
          const next = holder.firstElementChild;
          if (next) {
            author.replaceWith(next);
            bindAuthorProfileCard(next);
          }
          hydrateChatAvatar(record.avatarEl, name);
        }
      }
      const entry = sideEntries.find((item) => item.id === id);
      if (entry && entry.senderId === node && entry.kind !== "self") {
        entry.who = name;
        entry.accountKind = "user";
        side = true;
      }
    }
    if (side) renderSideMessages();
    if (thread || ids.includes(activeThreadRootId)) renderThreadDialog();
  }

  function isAuthenticatedPlain(plain) {
    return Boolean(frameAuth && (frameAuth.isSealedPlain(plain) || frameAuth.isServerPlain(plain)));
  }

  async function encryptRoomObject(framed) {
    const nonce = crypto.getRandomValues(new Uint8Array(12));
    const combined = new Uint8Array(
      await crypto.subtle.encrypt(
        { name: "AES-GCM", iv: nonce, tagLength: 128 },
        roomKey,
        enc.encode(JSON.stringify(framed))
      )
    );
    const body = combined.slice(0, combined.length - 16);
    const tag = combined.slice(combined.length - 16);
    return {
      kind: "cipher",
      v: 1,
      nonce: bytesToB64(nonce),
      tag: bytesToB64(tag),
      body: bytesToB64(body),
    };
  }

  async function encryptObject(obj) {
    if (frameAuth?.isPrivateRelayFrame(obj)) {
      throw new Error("Private frames do not go through a relay room.");
    }
    if (!roomSeal || !sealModule) {
      throw new Error("End-to-end chat encryption is unavailable.");
    }
    const rotation = roomSeal.rotation;
    const sealed = await sealModule.sealFrame(obj, roomSeal, ACTIVE_ROOM);
    if (roomSeal.rotation !== rotation) void sendSealAdvertisement(true);
    return encryptRoomObject(sealed);
  }

  let keyReplyTimer = 0;

  async function sendSealAdvertisement(persist = false) {
    if (!roomSeal || !sealModule ||
        !socket || socket.readyState !== WebSocket.OPEN) return;
    const envelope = await encryptRoomObject(
      await sealModule.sealKeyFrame(roomSeal),
    );
    if (persist) envelope.persist = true;
    if (socket && socket.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify(envelope));
    }
  }

  function replyWithSealKeySoon() {
    if (keyReplyTimer) return;
    keyReplyTimer = window.setTimeout(() => {
      keyReplyTimer = 0;
      void sendSealAdvertisement(false);
    }, 25);
  }

  async function deliverReleasedSealFrames(results) {
    const plains = [];
    for (const result of results || []) {
      const plain = sealModule.authenticatedPlain(result);
      if (!plain || frameAuth?.isPrivateRelayFrame(plain)) continue;
      const moderated = await moderateDashboardPlain(plain);
      if (moderated) plains.push(moderated);
    }
    for (const plain of await attributedPlains(plains)) handlePlain(plain, true);
    if (results?.length) finishHistoryReplay();
  }

  async function decryptObject(envelope, history = false) {
    if (!envelope || envelope.kind !== "cipher") return null;
    try {
      const nonce = b64ToBytes(envelope.nonce);
      const body = b64ToBytes(envelope.body);
      const tag = b64ToBytes(envelope.tag);
      const combined = new Uint8Array(body.length + tag.length);
      combined.set(body, 0);
      combined.set(tag, body.length);
      const plain = await crypto.subtle.decrypt(
        { name: "AES-GCM", iv: nonce, tagLength: 128 },
        roomKey,
        combined
      );
      const framed = JSON.parse(dec.decode(plain));
      if (sealModule?.isSealKeyFrame(framed)) {
        const accepted = roomSeal
          ? await sealModule.acceptSealKey(framed, roomSeal)
          : null;
        if (accepted?.isNew) replyWithSealKeySoon();
        if (accepted?.released?.length) {
          void deliverReleasedSealFrames(accepted.released);
        }
        return null;
      }
      if (framed?.type === "sealed" || framed?.type === "e2ee-key") {
        if (!sealModule?.isSealedFrame(framed)) return null;
      } else {
        if (envelope.srv === true && frameAuth) {
          const injected = frameAuth.serverPlain(framed);
          return injected && !frameAuth.isPrivateRelayFrame(injected) ? injected : null;
        }
        noteUnreadableSealedFrame("");
        return null;
      }
      const result = roomSeal
        ? await sealModule.unsealFrame(framed, roomSeal, ACTIVE_ROOM, { history })
        : { status: "notForMe" };
      if (result.status === "notForMe") noteUnreadableSealedFrame(framed.kid);
      const opened = result.status === "opened"
        ? sealModule.authenticatedPlain(result)
        : null;
      return opened && !frameAuth?.isPrivateRelayFrame(opened) ? opened : null;
    } catch (_) {
      return null;
    }
  }

  // Empty means an unencrypted/controller-authored frame was refused; a key id
  // means retained ciphertext was not addressed to this device.
  function noteUnreadableSealedFrame(keyId) {
    if (PUBLIC_WORLD_GENERAL && !keyId) return;
    if (unreadableSealKeyIds.has(keyId)) return;
    unreadableSealKeyIds.add(keyId);
    setStatus(
      keyId
        ? "End-to-end encrypted · some older messages predate this device"
        : "End-to-end encrypted · refused an unencrypted frame"
    );
  }

  function readSession() {
    try {
      return JSON.parse(localStorage.getItem("forkmesh.session") || "null");
    } catch (_) {
      return null;
    }
  }

  function writeSession(session) {
    try {
      const stored = session && typeof session === "object"
        ? {
            ...session,
            sessionToken: (
              location.protocol === "https:" && session.sessionToken
                ? "cookie"
                : session.sessionToken || ""
            ),
          }
        : session;
      localStorage.setItem("forkmesh.session", JSON.stringify(stored));
    } catch (_) {}
  }

  function isUserLikeSession(session) {
    if (!session || !session.nodeName) return false;
    if (session.kind === "user") return true;
    if (session.kind === "node") return false;
    return Boolean(session.email);
  }

  async function hydrateUserSession() {
    const session = readSession();
    if (isUserLikeSession(session)) {
      cachedUserSession = session;
      if (session.kind === "user") return session;
    } else if (!session || !session.nodeName || session.kind === "node") {
      cachedUserSession = null;
      return null;
    }
    try {
      const res = await fetch("/api/accounts/" + encodeURIComponent(session.nodeName), {
        headers: { accept: "application/json" },
      });
      if (!res.ok) return cachedUserSession;
      const body = await res.json();
      if (!body || body.kind !== "user") {
        cachedUserSession = null;
        return null;
      }
      cachedUserSession = {
        ...session,
        nodeName: body.nodeName || session.nodeName,
        email: body.email || session.email || "",
        status: body.status || session.status || "active",
        pubkey: body.pubkey || session.pubkey || "",
        emailVerified: Boolean(body.emailVerified),
        isAdmin: Boolean(body.isAdmin),
        adminUrl: body.adminUrl || session.adminUrl || "",
        solana: body.solana || session.solana || "",
        hasPayoutAddress: Boolean(body.hasPayoutAddress),
        avatarPng: body.avatarPng || session.avatarPng || "",
        avatarUpdatedAt: Number(body.avatarUpdatedAt) || 0,
        kind: "user",
        owner: body.owner || "",
        nodes: Array.isArray(body.nodes) ? body.nodes : [],
        accountSnapshotVersion: 1,
        at: Date.now(),
      };
      writeSession(cachedUserSession);
      return cachedUserSession;
    } catch (_) {
      return cachedUserSession;
    }
  }

  function userSession() {
    if (isUserLikeSession(cachedUserSession)) return cachedUserSession;
    const session = readSession();
    return isUserLikeSession(session) ? session : null;
  }

  function discordRequestHeaders() {
    const headers = new Headers({ accept: "application/json" });
    const token = String(userSession()?.sessionToken || "").trim();
    if (token && token !== "cookie") {
      headers.set("authorization", `Bearer ${token}`);
    }
    return headers;
  }

  function discordRetryAfterMs(value) {
    const raw = String(value || "").trim();
    if (!raw) return 0;
    const seconds = Number(raw);
    if (Number.isFinite(seconds)) {
      return Math.max(0, Math.min(DISCORD_BACKOFF_MAX_MS, seconds * 1000));
    }
    const retryAt = Date.parse(raw);
    return Number.isFinite(retryAt)
      ? Math.max(0, Math.min(DISCORD_BACKOFF_MAX_MS, retryAt - Date.now()))
      : 0;
  }

  function deferDiscordRefresh(retryAfter = "") {
    discordFailureCount = Math.min(8, discordFailureCount + 1);
    const exponential = Math.min(
      DISCORD_BACKOFF_MAX_MS,
      DISCORD_BACKOFF_BASE_MS * (2 ** (discordFailureCount - 1)),
    );
    const retryAfterMs = discordRetryAfterMs(retryAfter);
    discordBackoffUntil = Math.max(
      discordBackoffUntil,
      Date.now() + Math.max(exponential, retryAfterMs),
    );
  }

  async function discordJson(path) {
    let response;
    try {
      response = await fetch(path, {
        headers: discordRequestHeaders(),
        credentials: "same-origin",
        cache: "no-store",
      });
    } catch (error) {
      deferDiscordRefresh();
      throw error;
    }
    if (!response.ok) {
      const error = new Error(`Discord source unavailable (${response.status})`);
      error.status = response.status;
      const retryHeader = String(response.headers.get("retry-after") || "");
      const retrySeconds = Number(retryHeader);
      const retryDate = Date.parse(retryHeader);
      const requestedDelay = Number.isFinite(retrySeconds)
        ? Math.max(0, retrySeconds * 1000)
        : Number.isFinite(retryDate)
          ? Math.max(0, retryDate - Date.now())
          : 0;
      // Some edge/provider 429 and 503 responses omit Retry-After. Waiting
      // only for the ordinary one-minute poll in that case repeatedly fans
      // out across every organization and extends the rate limit.
      if (response.status === 429 || response.status >= 500) {
        discordBackoffAttempts = Math.min(6, discordBackoffAttempts + 1);
      }
      const fallbackDelay =
        response.status === 429
          ? Math.min(
              30 * 60_000,
              5 * 60_000 * (2 ** (discordBackoffAttempts - 1)),
            )
          : response.status >= 500
            ? Math.min(
                10 * 60_000,
                60_000 * (2 ** (discordBackoffAttempts - 1)),
              )
            : 0;
      error.retryAfterMs = Math.min(
        3_600_000,
        Math.max(requestedDelay, fallbackDelay),
      );
      if (error.retryAfterMs) {
        discordBackoffUntil = Math.max(
          discordBackoffUntil,
          Date.now() + error.retryAfterMs,
        );
      }
      throw error;
    }
    return response.json();
  }

  async function discoverDiscordSources() {
    if (Array.isArray(discordSources)) return discordSources;
    const catalog = await discordJson("/api/orgs");
    const organizations = (Array.isArray(catalog?.orgs) ? catalog.orgs : [])
      .slice(0, DISCORD_MAX_ORGANIZATIONS);
    const sources = [];
    // Connector discovery is deliberately sequential. A rate-limit response
    // from the first organization must prevent more doomed requests from being
    // launched at the same provider in the same tick.
    for (const record of organizations) {
      const organization = String(record?.name || "").trim();
      if (!organization) continue;
      try {
        const status = await discordJson(
          `/api/orgs/${encodeURIComponent(organization)}/discord`,
        );
        if (!status?.configured || status?.state !== "configured") continue;
        const channelIds = Array.isArray(status?.connector?.channelIds)
          ? status.connector.channelIds
          : [];
        const channelNames = new Map(
          (Array.isArray(status?.channels) ? status.channels : []).map((channel) => [
            String(channel?.id || ""),
            String(channel?.name || ""),
          ]),
        );
        sources.push(
          ...channelIds
            .slice(0, DISCORD_MAX_CHANNELS)
            .map((channelId) => ({
              organization,
              channelId: String(channelId || ""),
              channelName: channelNames.get(String(channelId || "")) || "",
            }))
            .filter((source) => source.channelId),
        );
      } catch (error) {
        if (error?.status === 429 || error?.status >= 500) throw error;
        // One unavailable optional connector must not hide healthy connectors
        // belonging to the same account.
      }
    }
    discordSources = sources;
    return discordSources;
  }

  async function refreshDiscordMessages() {
    if (
      discordRefreshRunning ||
      document.visibilityState === "hidden" ||
      Date.now() < discordBackoffUntil ||
      !userSession()
    ) return;
    discordRefreshRunning = true;
    try {
      const sources = await discoverDiscordSources();
      const messages = [];
      // Discord applies rate limits across related connector routes. Fetching
      // every channel concurrently turns one provider-wide 429 into a burst
      // of identical failures, so read channels serially and stop immediately
      // when the connector asks us to cool down.
      for (const source of sources) {
        const path =
          `/api/orgs/${encodeURIComponent(source.organization)}` +
          `/discord/messages?channelId=${encodeURIComponent(source.channelId)}`;
        try {
          const payload = await discordJson(path);
          const channelName = String(
            payload?.channel?.name || source.channelName || source.channelId,
          ).trim();
          messages.push(
            ...(Array.isArray(payload?.messages) ? payload.messages : []).map(
              (message) => ({
                ...message,
                organization: source.organization,
                channelName,
              }),
            ),
          );
        } catch (error) {
          if (error?.status === 429 || error?.status >= 500) throw error;
          // A missing channel is isolated; continue with the remaining
          // configured channels without putting the whole connector on ice.
        }
      }
      const orderedMessages = messages
        .filter((message) => message?.id && String(message?.content || "").trim())
        .sort((left, right) =>
          Date.parse(left?.createdAt || "") - Date.parse(right?.createdAt || ""));
      const visibleMessages = discordInitialMessagesLoaded
        ? orderedMessages
        : orderedMessages.slice(-DISCORD_MAX_INITIAL_MESSAGES);
      let appended = 0;
      let sliceStartedAt = performance.now();
      for (const message of visibleMessages) {
        if (performance.now() - sliceStartedAt >= DISCORD_APPEND_SLICE_MS) {
          await nextChatSlice();
          sliceStartedAt = performance.now();
        }
        if (appendDiscordMessage(message)) appended += 1;
      }
      discordBackoffAttempts = 0;
      discordInitialMessagesLoaded = true;
      if (!refreshFailed) {
        discordFailureCount = 0;
        discordBackoffUntil = 0;
      }
      if (appended) {
        console.info("[ForkMesh chat] Discord messages refreshed", {
          sources: sources.length,
          appended,
        });
      }
    } catch (error) {
      // Discord is optional. Keep the encrypted room usable and retry later.
      console.info("[ForkMesh chat] Discord refresh deferred", {
        reason: String(error?.message || "unavailable").slice(0, 160),
      });
      if (!Array.isArray(discordSources) || !discordSources.length) {
        discordSources = null;
      }
    } finally {
      discordRefreshRunning = false;
    }
  }

  function stopDiscordMessageRefresh() {
    if (!discordRefreshTimer) return;
    clearTimeout(discordRefreshTimer);
    discordRefreshTimer = 0;
  }

  function scheduleDiscordMessageRefresh() {
    stopDiscordMessageRefresh();
    if (document.visibilityState === "hidden" || !userSession()) return;
    const backoff = Math.max(0, discordBackoffUntil - Date.now());
    const jitter = Math.floor(Math.random() * DISCORD_REFRESH_JITTER_MS);
    discordRefreshTimer = window.setTimeout(async () => {
      discordRefreshTimer = 0;
      await refreshDiscordMessages();
      scheduleDiscordMessageRefresh();
    }, Math.max(DISCORD_REFRESH_MS + jitter, backoff));
  }

  function startDiscordMessageRefresh() {
    stopDiscordMessageRefresh();
    if (document.visibilityState === "hidden" || !userSession()) return;
    void refreshDiscordMessages();
    scheduleDiscordMessageRefresh();
  }

  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "hidden") {
      stopDiscordMessageRefresh();
      return;
    }
    startDiscordMessageRefresh();
  });

  function canJoinChat() {
    return PUBLIC_WORLD_GENERAL || Boolean(userSession());
  }

  function orgAgentIdentity(sender, senderId = "") {
    const names = [sender, senderId].map((value) =>
      String(value || "").trim().toLowerCase());
    return names.some((name) =>
      name === CLAUDE_SENDER_ID ||
      name === CODEX_SENDER_ID ||
      name === ORG_BOT_SENDER_ID);
  }

  function chatAccountKind() {
    return PUBLIC_WORLD_GENERAL && !userSession() ? "guest" : "user";
  }

  // Everyone in the public World room shows under the plain name they assert.
  // The old "World visitor · jett" prefix read as a second, different person
  // sitting next to the signed-in "jett", so it is stripped from anything that
  // still carries it (session names, replayed history frames).
  function publicWorldName(value) {
    return String(value || "")
      .replace(/^World visitor\s*·\s*/i, "")
      .trim()
      .slice(0, MAX_NAME) || "guest";
  }

  // The World floats each chat line above the speaker's avatar by matching the
  // sender against that avatar's presence name (world.js
  // handleWorldChatMessage). A signed-out visitor stands in the World as
  // "Guest ####" — derived from the same per-tab guest id this same-origin
  // iframe can read — so the embedded chat has to introduce itself under that
  // name or a guest's bubble never finds its avatar. Keep this in step with
  // guestId()/hashSuffix()/accountIdentity() in public/world/world.js.
  const WORLD_GUEST_ID_KEY = "forkmesh.world.guestId.v1";
  function worldGuestPresenceName() {
    if (requestedParams.get("worldEmbed") !== "1") return "";
    let guest = "";
    try {
      guest = String(sessionStorage.getItem(WORLD_GUEST_ID_KEY) || "");
    } catch (_) {
      guest = "";
    }
    if (!guest) return "";
    let hash = 0;
    for (const char of `guest:${guest}`) {
      hash = (Math.imul(hash, 31) + char.charCodeAt(0)) >>> 0;
    }
    return `Guest ${String(hash % 10000).padStart(4, "0")}`;
  }

  function displayName() {
    const session = userSession() || readSession();
    const value =
      session?.nodeName ||
      session?.email ||
      worldGuestPresenceName() ||
      `World Guest ${String(selfId).replace(/[^A-Za-z0-9]/g, "").slice(0, 6)}`;
    const name = String(value).trim().slice(0, MAX_NAME) || "World Guest";
    return PUBLIC_WORLD_GENERAL ? publicWorldName(name) : name;
  }

  function escapeHtml(value) {
    return String(value || "").replace(/[&<>"']/g, (char) => ({
      "&": "&amp;",
      "<": "&lt;",
      ">": "&gt;",
      '"': "&quot;",
      "'": "&#039;",
    })[char]);
  }

  function mentionName(value) {
    return String(value || "").trim().toLowerCase();
  }

  function mentionProfilePath(name) {
    const key = mentionName(name);
    if (!key) return "#";
    const official = new Set([
      "app.forkmesh.com", "forkmesh.com", "world.forkmesh.com",
    ]).has(location.hostname.toLowerCase());
    const origin = official ? "https://forkmesh.com" : location.origin;
    return origin + "/@" + encodeURIComponent(key);
  }

  function authorProfileName(record) {
    if (!record || record.external || record.accountKind !== "user") return "";
    const name = mentionName(record.who).replace(/^@/, "");
    if (!/^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(name)) return "";
    const bots = [FORKBOT_SENDER_ID, CLAUDE_SENDER_ID, CODEX_SENDER_ID, ORG_BOT_SENDER_ID];
    const ids = [name, mentionName(record.senderId)];
    return ids.some((id) => bots.includes(id)) ? "" : name;
  }

  function authorElementHtml(record, className, statusName = true) {
    const name = authorProfileName(record);
    const label = escapeHtml(record.who);
    const status = statusName && !["guest", "integration"].includes(record.accountKind)
      ? `data-user-status-name="${label}"`
      : "";
    if (!name) return `<span class="${className}" ${status}>${label}</span>`;
    return `<a class="${className} hover:underline" href="${escapeHtml(mentionProfilePath(name))}" data-chat-author-profile="${escapeHtml(name)}" ${status}>${label}</a>`;
  }

  function bindAuthorProfileCard(element) {
    const name = element?.dataset?.chatAuthorProfile;
    if (!name) return;
    element.addEventListener("mouseenter", () => showMentionCard(element, name));
    element.addEventListener("focus", () => showMentionCard(element, name));
    element.addEventListener("mouseleave", hideMentionCardSoon);
    element.addEventListener("blur", hideMentionCardSoon);
  }

  async function fetchMentionProfile(name) {
    const key = mentionName(name);
    if (!key) return null;
    if (mentionProfileCache.has(key)) return mentionProfileCache.get(key);
    const session = userSession();
    if (
      session?.nodeName?.toLowerCase() === key &&
      Number(session.accountSnapshotVersion) >= 1
    ) {
      const ownProfile = Promise.resolve(session);
      mentionProfileCache.set(key, ownProfile);
      return ownProfile;
    }
    const pending = fetch("/api/accounts/" + encodeURIComponent(key), {
      headers: { accept: "application/json" },
    }).then(async (response) => {
      if (!response.ok) return null;
      const profile = await response.json().catch(() => null);
      if (!profile || profile.exists === false || profile.kind !== "user") return null;
      return profile;
    }).catch(() => null);
    mentionProfileCache.set(key, pending);
    return pending;
  }

  function mentionSummary(profile) {
    if (!profile) return "User profile";
    const bio = String(profile.profileBio || "").trim();
    if (bio) return bio.length > 120 ? bio.slice(0, 117) + "..." : bio;
    const nodes = Array.isArray(profile.nodes)
      ? profile.nodes.map((node) => String(node || "").trim()).filter(Boolean)
      : [];
    if (nodes.length) return "Nodes: " + nodes.slice(0, 3).join(", ");
    return profile.status ? String(profile.status) : "User profile";
  }

  function ensureMentionCard() {
    if (mentionCardEl) return mentionCardEl;
    mentionCardEl = document.createElement("a");
    mentionCardEl.className = "chat-mention-card";
    mentionCardEl.hidden = true;
    mentionCardEl.addEventListener("mouseenter", () => {
      if (mentionHideTimer) clearTimeout(mentionHideTimer);
    });
    mentionCardEl.addEventListener("mouseleave", hideMentionCardSoon);
    document.body.append(mentionCardEl);
    return mentionCardEl;
  }

  function renderMentionCard(card, name, profile) {
    const display = String(profile?.name || profile?.nodeName || name || "").trim() || name;
    card.textContent = "";
    card.href = mentionProfilePath(name);
    const title = document.createElement("strong");
    title.textContent = "@" + display;
    const summary = document.createElement("span");
    summary.textContent = mentionSummary(profile);
    const action = document.createElement("small");
    action.textContent = "Click to open full profile";
    card.append(title, summary, action);
  }

  function positionMentionCard(anchor, card) {
    const rect = anchor.getBoundingClientRect();
    const margin = 12;
    const width = card.offsetWidth || 240;
    const x = Math.max(margin, Math.min(rect.left, window.innerWidth - width - margin));
    const y = Math.min(rect.bottom + 8, window.innerHeight - card.offsetHeight - margin);
    card.style.left = `${x + window.scrollX}px`;
    card.style.top = `${Math.max(margin, y) + window.scrollY}px`;
  }

  function hideMentionCardSoon() {
    if (mentionHideTimer) clearTimeout(mentionHideTimer);
    mentionHideTimer = setTimeout(() => {
      if (mentionCardEl) mentionCardEl.hidden = true;
      activeMentionAnchor = null;
    }, 140);
  }

  async function showMentionCard(anchor, name) {
    const key = mentionName(name);
    if (!key) return;
    if (mentionHideTimer) clearTimeout(mentionHideTimer);
    activeMentionAnchor = anchor;
    const card = ensureMentionCard();
    renderMentionCard(card, key, null);
    card.hidden = false;
    positionMentionCard(anchor, card);
    const profile = await fetchMentionProfile(key);
    if (activeMentionAnchor !== anchor) return;
    renderMentionCard(card, key, profile);
    positionMentionCard(anchor, card);
  }

  function appendMentionText(container, text) {
    const value = String(text || "");
    const mentionRe = new RegExp(CHAT_MENTION_RE.source, "gi");
    let cursor = 0;
    let match;
    while ((match = mentionRe.exec(value)) !== null) {
      const prefix = match[1] || "";
      const name = mentionName(match[2]);
      const start = match.index + prefix.length;
      const end = mentionRe.lastIndex;
      if (start > cursor) container.append(document.createTextNode(value.slice(cursor, start)));
      const anchor = document.createElement("a");
      anchor.className = "chat-mention";
      anchor.href = mentionProfilePath(name);
      anchor.dataset.chatMention = name;
      anchor.textContent = value.slice(start, end);
      anchor.addEventListener("mouseenter", () => showMentionCard(anchor, name));
      anchor.addEventListener("focus", () => showMentionCard(anchor, name));
      anchor.addEventListener("mouseleave", hideMentionCardSoon);
      anchor.addEventListener("blur", hideMentionCardSoon);
      container.append(anchor);
      cursor = end;
    }
    if (cursor < value.length) container.append(document.createTextNode(value.slice(cursor)));
  }

  // ---- links inside message text -----------------------------------------
  // A URL belongs in the sentence that explains it, not in a message of its
  // own. Both shapes people write are resolved here — a pasted http(s) URL and
  // markdown's [label](url) — with the same rules the standalone chat page
  // uses (chat-rich-text.js), so one message links the same way on either
  // surface. This bundle is a classic script and cannot import that module.
  const SAFE_CHAT_LINK_PROTOCOLS = new Set(["http:", "https:", "mailto:"]);

  function safeChatLink(value) {
    try {
      const url = new URL(String(value || "").trim(), window.location.href);
      return SAFE_CHAT_LINK_PROTOCOLS.has(url.protocol) ? url.href : "";
    } catch (_) {
      return "";
    }
  }

  // The bare URL starting at `index`, without the sentence punctuation that
  // trails it. A closing parenthesis counts as part of the address only when
  // the address opened one: "…/Foo_(bar)" is a page, "(see …/status)" is not.
  function rawChatUrlAt(source, index) {
    const match = source.slice(index).match(/^https?:\/\/[^\s<>]+/i);
    if (!match) return "";
    let url = match[0];
    while (url) {
      const last = url[url.length - 1];
      if (",.!?;:".includes(last)) {
        url = url.slice(0, -1);
        continue;
      }
      if (last === ")" && url.split(")").length > url.split("(").length) {
        url = url.slice(0, -1);
        continue;
      }
      break;
    }
    return url;
  }

  function chatLinkTargetEnd(source, start) {
    let depth = 0;
    for (let index = start; index < source.length; index += 1) {
      if (source[index] === "\n") return -1; // a link never spans lines
      if (source[index] === "(") {
        depth += 1;
        continue;
      }
      if (source[index] !== ")") continue;
      if (depth === 0) return index;
      depth -= 1;
    }
    return -1;
  }

  function appendChatLink(container, label, href) {
    const anchor = document.createElement("a");
    anchor.className = "chat-link";
    anchor.href = href;
    anchor.target = "_blank";
    anchor.rel = "noopener noreferrer";
    anchor.textContent = label;
    container.append(anchor);
  }

  function renderMessageText(container, text) {
    container.textContent = "";
    const value = String(text || "");
    let cursor = 0;
    let index = 0;
    while (index < value.length) {
      if (value[index] === "[") {
        const labelEnd = value.indexOf("](", index + 1);
        const targetEnd =
          labelEnd > index + 1 && !value.slice(index, labelEnd).includes("\n")
            ? chatLinkTargetEnd(value, labelEnd + 2)
            : -1;
        if (targetEnd > labelEnd + 1) {
          const label = value.slice(index + 1, labelEnd);
          const href = safeChatLink(value.slice(labelEnd + 2, targetEnd));
          if (href) {
            appendMentionText(container, value.slice(cursor, index));
            appendChatLink(container, label, href);
          } else {
            // A target we will not open: show the words the author wrote
            // rather than a dead link.
            appendMentionText(container, value.slice(cursor, index) + label);
          }
          index = targetEnd + 1;
          cursor = index;
          continue;
        }
      }
      const rawUrl = rawChatUrlAt(value, index);
      const rawHref = rawUrl ? safeChatLink(rawUrl) : "";
      if (rawHref) {
        appendMentionText(container, value.slice(cursor, index));
        appendChatLink(container, rawUrl, rawHref);
        index += rawUrl.length;
        cursor = index;
        continue;
      }
      index += 1;
    }
    appendMentionText(container, value.slice(cursor));
  }

  // ---- @mention autocomplete ---------------------------------------------
  // Typing "@" (plus an optional partial name) in the composer pops a
  // suggestion list; Tab (or Enter / click) accepts the highlighted name,
  // arrows move, Escape dismisses. Mirrors the full chat page composer
  // (chat.js) so both surfaces mention the same way — and so "@forkbot" is
  // reachable without typing it exactly.
  const MENTION_DIRECTORY_ENDPOINT = "/api/accounts/users";
  const MENTION_DIRECTORY_TTL_MS = 60000;
  const MENTION_LIMIT = 8;
  const MENTION_STALE_MS = 180000;
  // Only names the renderer would actually link (CHAT_MENTION_RE) are worth
  // suggesting: guest display names carry spaces and never become mentions.
  const MENTIONABLE_NAME_RE = /^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/;

  function rememberMentionPerson(name, tsMs) {
    if (frameAuth?.isBotViaName(name)) return;
    const key = mentionName(name);
    if (!key || !MENTIONABLE_NAME_RE.test(key)) return;
    const seenMs = Number(tsMs) || 0;
    const prev = mentionPeople.get(key);
    if (prev && prev.lastSeenMs >= seenMs) return;
    mentionPeople.set(key, {
      name: key,
      kind: key === FORKBOT_SENDER_ID ? "bot" : "user",
      lastSeenMs: seenMs,
    });
    scheduleMemberList();
  }

  // Registered accounts, so someone who has not spoken in this room yet is
  // still mentionable. Fetched on the first "@" only (an idle dashboard tab
  // never pays for it) and refreshed at most once a minute. Resolves true when
  // it added names, so an open popup can re-render with them.
  function refreshMentionDirectory() {
    if (mentionDirectoryPending) return mentionDirectoryPending;
    if (
      mentionDirectoryFetchedMs &&
      Date.now() - mentionDirectoryFetchedMs < MENTION_DIRECTORY_TTL_MS
    ) {
      return Promise.resolve(false);
    }
    mentionDirectoryPending = fetch(MENTION_DIRECTORY_ENDPOINT, {
      headers: { accept: "application/json" },
    })
      .then((response) => (response.ok ? response.json() : null))
      .catch(() => null)
      .then((data) => {
        mentionDirectoryFetchedMs = Date.now();
        mentionDirectoryPending = null;
        const users = data && Array.isArray(data.users) ? data.users : [];
        let added = false;
        for (const user of users) {
          const key = mentionName(user && user.name);
          if (!key || !MENTIONABLE_NAME_RE.test(key)) continue;
          window.ForkMeshUserStatus?.remember(key, user.userStatus);
          if (mentionDirectory.has(key)) continue;
          mentionDirectory.set(key, { name: key, kind: "user", lastSeenMs: 0 });
          added = true;
        }
        if (added) scheduleMemberList();
        return added;
      });
    return mentionDirectoryPending;
  }

  // The "@partial" token the caret sits inside, or null. A mention starts at an
  // "@" preceded by whitespace/start and uses the charset the renderer links.
  function mentionTokenAtCaret(inputEl) {
    if (!inputEl) return null;
    const caret = inputEl.selectionStart;
    if (caret == null || inputEl.selectionEnd !== caret) return null;
    const before = String(inputEl.value || "").slice(0, caret);
    const match = /(^|\s)@([A-Za-z0-9-]{0,32})$/.exec(before);
    if (!match) return null;
    return {
      start: caret - match[2].length - 1, // include the "@"
      end: caret,
      partial: match[2].toLowerCase(),
    };
  }

  function mentionIsOnline(person) {
    return Date.now() - (person.lastSeenMs || 0) < MENTION_STALE_MS;
  }

  function mentionCandidates(partial) {
    const self = mentionName(displayName());
    const byName = new Map();
    for (const person of mentionDirectory.values()) byName.set(person.name, person);
    // Room presence wins over the directory entry: it carries a lastSeenMs.
    for (const person of mentionPeople.values()) byName.set(person.name, person);
    byName.set(FORKBOT_SENDER_ID, {
      name: FORKBOT_SENDER_ID,
      kind: "bot",
      lastSeenMs: Date.now(),
    });
    if (orgAgentQueueAccess) {
      for (const name of [CLAUDE_SENDER_ID, CODEX_SENDER_ID]) {
        byName.set(name, {
          name,
          kind: "bot",
          lastSeenMs: Date.now(),
        });
      }
    } else {
      byName.delete(CLAUDE_SENDER_ID);
      byName.delete(CODEX_SENDER_ID);
    }
    byName.delete(self);
    const matches = [...byName.values()].filter(
      (person) => !partial || person.name.includes(partial));
    matches.sort((a, b) => {
      const aPrefix = partial && a.name.startsWith(partial) ? 0 : 1;
      const bPrefix = partial && b.name.startsWith(partial) ? 0 : 1;
      return (aPrefix - bPrefix) ||
        (mentionIsOnline(b) - mentionIsOnline(a)) ||
        a.name.localeCompare(b.name);
    });
    return matches.slice(0, MENTION_LIMIT);
  }

  function ensureMentionSuggest(inputEl) {
    if (!mentionSuggestEl) {
      mentionSuggestEl = document.createElement("div");
      mentionSuggestEl.className =
        "fm-popover fm-menu absolute left-0 right-0 z-30 mb-2 overflow-y-auto";
      // bottom/max-height inline: the dashboard ships a pre-compiled Tailwind
      // subset that has no bottom-full / max-h-56 rule to lean on.
      mentionSuggestEl.style.bottom = "100%";
      mentionSuggestEl.style.maxHeight = "224px";
      mentionSuggestEl.hidden = true;
      mentionSuggestEl.setAttribute("role", "listbox");
      mentionSuggestEl.setAttribute("aria-label", "Mention a user");
    }
    // Anchor to the composer pill the input lives in, so the list floats over
    // the transcript instead of pushing the composer around.
    const host = inputEl.parentElement;
    if (host && mentionSuggestEl.parentElement !== host) {
      host.classList.add("relative");
      host.append(mentionSuggestEl);
    }
    return mentionSuggestEl;
  }

  function ensureSlashSuggest(inputEl) {
    if (!slashSuggestEl) {
      slashSuggestEl = document.createElement("div");
      slashSuggestEl.id = "dashboard-chat-slash-suggest";
      slashSuggestEl.className =
        "fm-popover fm-menu absolute left-0 right-0 z-30 mb-2 overflow-y-auto";
      slashSuggestEl.style.bottom = "100%";
      slashSuggestEl.style.maxHeight = "224px";
      slashSuggestEl.hidden = true;
      slashSuggestEl.setAttribute("role", "listbox");
      slashSuggestEl.setAttribute("aria-label", "Available slash commands");
    }
    const host = inputEl.parentElement;
    if (host && slashSuggestEl.parentElement !== host) {
      host.classList.add("relative");
      host.append(slashSuggestEl);
    }
    return slashSuggestEl;
  }

  function closeSlashSuggest() {
    const inputEl = slashSuggest?.input;
    slashSuggest = null;
    if (slashSuggestEl) slashSuggestEl.hidden = true;
    inputEl?.setAttribute("aria-expanded", "false");
  }

  function slashTokenAtCaret(inputEl) {
    const caret = inputEl?.selectionStart;
    if (
      caret == null ||
      inputEl.selectionEnd !== caret ||
      caret !== inputEl.value.length
    ) return null;
    const match = /^\/([A-Za-z]*)$/.exec(inputEl.value);
    return match ? match[1].toLowerCase() : null;
  }

  // Suggestion rows are picked by pointer, not by mouse alone: "pointerdown"
  // covers mouse, pen, and touch, "mousedown" covers engines without pointer
  // events, and "click" covers the ones that swallow the synthetic mousedown a
  // tap produces. preventDefault keeps focus - and the caret - in the composer,
  // so no blur fires to close the popup underneath the press. Accepting is a
  // no-op once the popup has closed, so the events that follow the first one
  // cannot insert a second name.
  function wireSuggestPick(item, accept) {
    const pick = (event) => {
      if (event.cancelable) event.preventDefault();
      accept();
    };
    item.addEventListener("pointerdown", pick);
    item.addEventListener("mousedown", pick);
    item.addEventListener("click", pick);
  }

  // Move the highlight in place instead of re-rendering. Rebuilding the list
  // from a hover handler detaches the row under the cursor; the browser
  // re-fires mouseenter on its replacement on the next frame, which rebuilds
  // again, so the row churns every frame and a press lands on a node that is
  // already gone.
  function highlightSuggestItems(box, index, scroll) {
    if (!box) return;
    box.querySelectorAll('[role="option"]').forEach((el, i) => {
      const active = i === index;
      el.setAttribute("aria-selected", active ? "true" : "false");
      el.classList.toggle("bg-secondary", active);
      el.classList.toggle("text-foreground", active);
      el.classList.toggle("text-muted-foreground", !active);
      el.classList.toggle("hover:bg-secondary/60", !active);
      if (active && scroll) el.scrollIntoView({ block: "nearest" });
    });
  }

  function highlightSlashSuggest(index, scroll) {
    if (!slashSuggest) return;
    const count = slashSuggest.items.length;
    if (!count) return;
    slashSuggest.index = ((index % count) + count) % count;
    highlightSuggestItems(slashSuggestEl, slashSuggest.index, scroll);
  }

  function highlightMentionSuggest(index, scroll) {
    if (!mentionSuggest) return;
    const count = mentionSuggest.items.length;
    if (!count) return;
    mentionSuggest.index = ((index % count) + count) % count;
    highlightSuggestItems(mentionSuggestEl, mentionSuggest.index, scroll);
  }

  function renderSlashSuggest() {
    if (!slashSuggest) return;
    const box = ensureSlashSuggest(slashSuggest.input);
    box.textContent = "";
    const hint = document.createElement("div");
    hint.className = "fm-menu-heading";
    hint.textContent = "Available commands · Tab to insert · Esc to dismiss";
    box.append(hint);
    slashSuggest.items.forEach((entry, index) => {
      const active = index === slashSuggest.index;
      const item = document.createElement("button");
      item.type = "button";
      item.setAttribute("role", "option");
      item.setAttribute("aria-selected", active ? "true" : "false");
      item.className =
        "flex h-menu-row w-full items-center gap-3 rounded-md px-2 text-left text-control " +
        (active
          ? "bg-secondary text-foreground"
          : "text-muted-foreground hover:bg-secondary/60");
      const command = document.createElement("span");
      command.className = "shrink-0 font-mono font-semibold text-primary";
      command.textContent = entry.usage;
      const description = document.createElement("span");
      description.className = "min-w-0 truncate";
      description.textContent = entry.description;
      item.append(command, description);
      wireSuggestPick(item, () => acceptSlashSuggest(index));
      item.addEventListener("mouseenter", () => highlightSlashSuggest(index, false));
      box.append(item);
    });
    box.hidden = false;
    slashSuggest.input.setAttribute("aria-expanded", "true");
  }

  function updateSlashSuggest(inputEl) {
    const partial = slashTokenAtCaret(inputEl);
    if (partial == null) {
      closeSlashSuggest();
      return;
    }
    const items = SLASH_COMMANDS.filter((entry) =>
      entry.command.slice(1).startsWith(partial));
    if (!items.length) {
      closeSlashSuggest();
      return;
    }
    const previous = slashSuggest?.items[slashSuggest.index]?.command;
    let index = previous
      ? items.findIndex((entry) => entry.command === previous)
      : 0;
    if (index < 0) index = 0;
    slashSuggest = { input: inputEl, items, index };
    closeMentionSuggest();
    renderSlashSuggest();
  }

  function acceptSlashSuggest(index) {
    if (!slashSuggest) return;
    const inputEl = slashSuggest.input;
    const entry = slashSuggest.items[
      typeof index === "number" ? index : slashSuggest.index];
    if (!entry || !inputEl) return;
    inputEl.value = entry.command + " ";
    closeSlashSuggest();
    inputEl.focus();
    inputEl.setSelectionRange(inputEl.value.length, inputEl.value.length);
  }

  function moveSlashSuggest(step) {
    if (!slashSuggest) return;
    highlightSlashSuggest(slashSuggest.index + step, true);
  }

  function closeMentionSuggest() {
    mentionSuggest = null;
    if (mentionSuggestEl) mentionSuggestEl.hidden = true;
  }

  function renderMentionSuggest() {
    if (!mentionSuggest) return;
    const box = ensureMentionSuggest(mentionSuggest.input);
    box.textContent = "";
    const hint = document.createElement("div");
    hint.className = "fm-menu-heading";
    hint.textContent = "Tab to mention · Esc to dismiss";
    box.append(hint);
    mentionSuggest.items.forEach((person, index) => {
      const active = index === mentionSuggest.index;
      const item = document.createElement("button");
      item.type = "button";
      item.setAttribute("role", "option");
      item.setAttribute("aria-selected", active ? "true" : "false");
      item.className =
        "flex h-menu-row w-full items-center gap-2 rounded-md px-2 text-left text-control " +
        (active
          ? "bg-secondary text-foreground"
          : "text-muted-foreground hover:bg-secondary/60");
      const avatar = document.createElement("span");
      avatar.className =
        "flex h-5 w-5 shrink-0 items-center justify-center rounded-full border " +
        "border-border bg-secondary font-mono text-[10px] font-semibold text-foreground";
      avatar.textContent = person.name.slice(0, 1).toUpperCase();
      const label = document.createElement("span");
      label.className = "truncate font-semibold";
      label.textContent = "@" + person.name;
      item.append(avatar, label);
      if (mentionIsOnline(person)) {
        const dot = document.createElement("span");
        dot.className = "ml-auto h-1.5 w-1.5 shrink-0 rounded-full bg-success";
        dot.title = "Active in this room";
        item.append(dot);
      }
      wireSuggestPick(item, () => acceptMentionSuggest(index));
      item.addEventListener("mouseenter", () => highlightMentionSuggest(index, false));
      box.append(item);
    });
    box.hidden = false;
    const activeItem = box.querySelector('[aria-selected="true"]');
    if (activeItem) activeItem.scrollIntoView({ block: "nearest" });
  }

  function showMentionSuggest(inputEl) {
    const token = mentionTokenAtCaret(inputEl);
    if (!token) {
      closeMentionSuggest();
      return;
    }
    const items = mentionCandidates(token.partial);
    if (!items.length) {
      closeMentionSuggest();
      return;
    }
    const prev = mentionSuggest && mentionSuggest.items[mentionSuggest.index];
    let index = prev ? items.findIndex((person) => person.name === prev.name) : 0;
    if (index < 0) index = 0;
    mentionSuggest = { input: inputEl, items, index, start: token.start, end: token.end };
    renderMentionSuggest();
  }

  function updateMentionSuggest(inputEl) {
    if (slashTokenAtCaret(inputEl) != null) {
      closeMentionSuggest();
      return;
    }
    if (!mentionTokenAtCaret(inputEl)) {
      closeMentionSuggest();
      return;
    }
    refreshMentionDirectory().then((added) => {
      if (added && mentionSuggest && mentionSuggest.input === inputEl) {
        showMentionSuggest(inputEl);
      }
    });
    showMentionSuggest(inputEl);
  }

  function acceptMentionSuggest(index) {
    if (!mentionSuggest) return;
    const inputEl = mentionSuggest.input;
    const person = mentionSuggest.items[
      typeof index === "number" ? index : mentionSuggest.index];
    if (!person || !inputEl) return;
    const value = String(inputEl.value || "");
    const inserted = "@" + person.name + " ";
    inputEl.value =
      value.slice(0, mentionSuggest.start) + inserted + value.slice(mentionSuggest.end);
    const caret = mentionSuggest.start + inserted.length;
    closeMentionSuggest();
    inputEl.setSelectionRange(caret, caret);
    inputEl.focus();
  }

  function moveMentionSuggest(step) {
    if (!mentionSuggest) return;
    highlightMentionSuggest(mentionSuggest.index + step, true);
  }

  function setStatus(text) {
    document.querySelectorAll("[data-dashboard-chat-status]").forEach((el) => {
      el.textContent = text;
    });
    syncChatUserPanel(Boolean(socket && socket.readyState === WebSocket.OPEN));
  }

  function fullEmptyHtml() {
    return `<div class="fm-blankslate" data-chat-empty><svg aria-hidden="true" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"></path></svg><span class="fm-blankslate-title">No messages yet</span><p class="fm-blankslate-desc">Type below to join the encrypted ${escapeHtml(CHANNEL)} collaboration channel.</p></div>`;
  }

  function sideEmptyHtml() {
    return `<div class="px-3 py-2 text-xs text-muted-foreground" data-chat-empty>Type to join ${escapeHtml(CHANNEL)}.</div>`;
  }

  function fullUserOnlyHtml() {
    return `<div class="fm-blankslate"><svg aria-hidden="true" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="11" width="18" height="11" rx="2"></rect><path d="M7 11V7a5 5 0 0 1 10 0v4"></path></svg><span class="fm-blankslate-title">Sign in to chat here</span><p class="fm-blankslate-desc">Log in as a user to join the authenticated ${escapeHtml(CHANNEL)} channel. Guests can use only public World #general.</p></div>`;
  }

  function sideUserOnlyHtml() {
    return '<div class="px-3 py-2 text-xs text-muted-foreground">User login required for this channel. Guests can use only public World #general.</div>';
  }

  function setInputsEnabled(enabled) {
    [fullInput, sideInput, fullSend, sideSend].forEach((el) => {
      if (el) el.disabled = !enabled;
    });
    attachmentControls.forEach((control) => {
      control.button.disabled = !enabled;
      control.input.disabled = !enabled;
    });
  }

  function showUserOnlyState() {
    setStatus("User login required for this channel");
    setInputsEnabled(false);
    if (fullLog) fullLog.innerHTML = fullUserOnlyHtml();
    if (sideLog) sideLog.innerHTML = sideUserOnlyHtml();
  }

  function ensureEmptyState() {
    if (!canJoinChat()) {
      showUserOnlyState();
      return;
    }
    setInputsEnabled(true);
    if (fullLog && !fullLog.children.length) fullLog.innerHTML = fullEmptyHtml();
    if (sideLog && !sideLog.children.length) sideLog.innerHTML = sideEmptyHtml();
  }

  function clearEmptyState() {
    [fullLog, sideLog].forEach((log) => {
      if (!log) return;
      for (const child of log.children) {
        if (child.hasAttribute("data-chat-empty")) {
          log.textContent = "";
          return;
        }
      }
    });
  }

  function generatedChatFace(handle) {
    const faces = ["🙂", "😄", "😊", "🤓", "🧐", "😎", "😁", "🤠"];
    let hash = 2166136261;
    for (const char of String(handle || "guest").toLowerCase()) {
      hash ^= char.charCodeAt(0);
      hash = Math.imul(hash, 16777619) >>> 0;
    }
    return faces[hash % faces.length];
  }

  function publicChatAvatar(handle) {
    const name = String(handle || "").trim().toLowerCase();
    const session = userSession();
    if (
      session?.nodeName?.toLowerCase() === name &&
      Number(session.accountSnapshotVersion) >= 1
    ) {
      return Promise.resolve(String(session.avatarPng));
    }
    if (!/^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(name)) {
      return Promise.resolve("");
    }
    if (chatAvatarCache.has(name)) return chatAvatarCache.get(name);
    const pending = fetch(`/api/accounts/${encodeURIComponent(name)}`, {
      headers: { accept: "application/json" },
    })
      .then((response) => (response.ok ? response.json() : null))
      .then((profile) => {
        const avatar = String(profile?.avatarPng || "");
        return (
          profile?.exists === true &&
          profile?.profilePrivate !== true &&
          avatar.length <= 350_000 &&
          /^[A-Za-z0-9+/=]+$/.test(avatar)
        )
          ? avatar
          : "";
      })
      .catch(() => "");
    chatAvatarCache.set(name, pending);
    return pending;
  }

  function hydrateChatAvatar(avatar, handle) {
    if (!avatar) return;
    const name = String(handle || "guest").trim() || "guest";
    avatar.textContent =
      ["forkbot", "claude", "codex"].includes(name.toLowerCase())
        ? "🤖"
        : generatedChatFace(name);
    avatar.setAttribute("aria-label", `${name} avatar`);
    void publicChatAvatar(name).then((avatarPng) => {
      if (!avatar.isConnected || !avatarPng) return;
      const image = document.createElement("img");
      image.src = `data:image/png;base64,${avatarPng}`;
      image.alt = "";
      image.className = "h-full w-full rounded-full object-cover";
      avatar.replaceChildren(image);
    });
  }

  function fmtChatTime(tsMs) {
    const value = Number(tsMs);
    if (!value) return "";
    return new Date(value).toLocaleTimeString("en-US", {
      hour: "numeric", minute: "2-digit", hour12: true,
    }).toLowerCase().replace(/\s+/g, "");
  }

  let imagePreviewDialog = null;

  function openImagePreview(src, fileName) {
    if (!imagePreviewDialog) {
      imagePreviewDialog = document.createElement("dialog");
      imagePreviewDialog.id = "dashboard-chat-image-preview-dialog";
      imagePreviewDialog.className = "chat-image-preview-dialog max-h-[calc(100vh-1.5rem)] max-w-[calc(100vw-2rem)] rounded-xl border border-border bg-background p-3 shadow-2xl";
      imagePreviewDialog.innerHTML = '<button type="button" class="fm-btn fm-btn-icon fm-btn-sm absolute right-0 top-0" aria-label="Close image preview"><svg aria-hidden="true" width="16" height="16" viewBox="0 0 16 16" fill="currentColor"><path d="M3.72 3.72a.75.75 0 0 1 1.06 0L8 6.94l3.22-3.22a.749.749 0 0 1 1.275.326.749.749 0 0 1-.215.734L9.06 8l3.22 3.22a.749.749 0 0 1-.326 1.275.749.749 0 0 1-.734-.215L8 9.06l-3.22 3.22a.751.751 0 0 1-1.042-.018.751.751 0 0 1-.018-1.042L6.94 8 3.72 4.78a.75.75 0 0 1 0-1.06Z"></path></svg></button><img class="chat-image-preview-full max-h-[calc(100vh-1.5rem)] max-w-full object-contain" />';
      imagePreviewDialog.querySelector("button").addEventListener("click", () => imagePreviewDialog.close());
      imagePreviewDialog.addEventListener("click", (event) => {
        if (event.target === imagePreviewDialog) imagePreviewDialog.close();
      });
      document.body.append(imagePreviewDialog);
    }
    const image = imagePreviewDialog.querySelector("img");
    image.src = src;
    image.alt = fileName;
    if (!imagePreviewDialog.open) imagePreviewDialog.showModal();
  }

  // Built on its own so the video player can fall back to it when the browser
  // turns out not to decode that file.
  function attachmentFileCard(attachment, objectUrl, compact = false) {
    const card = document.createElement("div");
    card.className = "chat-attachment-card";
    card.className += compact
      ? " flex min-w-0 items-center gap-2 rounded-md border border-border bg-secondary/70 px-2 py-1.5"
      : " flex min-w-0 items-center gap-3 rounded-lg border border-border bg-secondary px-3 py-2";
    const info = document.createElement("div");
    info.className = "min-w-0 flex-1";
    const name = document.createElement("div");
    name.className = "truncate text-xs font-semibold text-foreground";
    name.textContent = attachment.fileName;
    name.title = attachment.fileName;
    const meta = document.createElement("div");
    meta.className = "truncate text-[10px] text-muted-foreground";
    meta.textContent = `${attachment.fileMime} - ${formatAttachmentSize(attachment.size)}`;
    info.append(name, meta);
    const link = document.createElement("a");
    link.className = "shrink-0 text-[11px] font-semibold text-primary hover:underline";
    link.href = objectUrl;
    link.download = attachment.fileName;
    link.textContent = "Download";
    link.setAttribute("aria-label", `Download ${attachment.fileName}`);
    card.append(info, link);
    return card;
  }

  function renderAttachment(attachment, compact = false) {
    if (!attachment) return null;
    const objectUrl = attachmentObjectUrl(attachment);
    const wrapper = document.createElement("div");
    wrapper.className = "chat-attachment-wrapper";
    wrapper.className += compact
      ? " chat-attachment-wrapper--compact mt-1 grid gap-1.5"
      : " mt-2 grid max-w-md gap-2";
    if (attachmentBlobType(attachment.fileMime).startsWith("image/")) {
      const preview = document.createElement("div");
      preview.className = "chat-attachment-preview relative w-fit max-w-full";
      const image = document.createElement("img");
      image.className = "chat-attachment-image";
      image.className += compact
        ? " max-h-28 max-w-full rounded-md border border-border object-contain"
        : " max-h-72 max-w-full rounded-lg border border-border bg-secondary object-contain";
      image.src = objectUrl;
      image.alt = attachment.fileName;
      // History rows are already materialized lazily. Deferring the image a
      // second time leaves a reserved but empty card when a fast scroll brings
      // that row on screen, especially in the composited World panel.
      image.loading = "eager";
      image.decoding = "async";
      image.style.minWidth = compact
        ? "min(72px, 100%)"
        : "min(96px, 100%)";
      image.style.minHeight = compact ? "54px" : "72px";
      image.tabIndex = 0;
      image.setAttribute("role", "button");
      image.setAttribute("aria-label", `Open ${attachment.fileName} full size`);
      image.addEventListener("click", () => openImagePreview(objectUrl, attachment.fileName));
      image.addEventListener("keydown", (event) => {
        if (event.key === "Enter" || event.key === " ") {
          event.preventDefault();
          openImagePreview(objectUrl, attachment.fileName);
        }
      });
      const download = document.createElement("a");
      download.href = objectUrl;
      download.download = attachment.fileName;
      download.setAttribute("aria-label", `Download ${attachment.fileName}`);
      download.title = `Download ${attachment.fileName}`;
      download.className = "chat-attachment-download absolute bottom-2 right-2 inline-flex h-7 w-7 items-center justify-center rounded-md border border-border bg-background/90 text-muted-foreground shadow-sm hover:text-foreground";
      download.innerHTML = '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 3v12m0 0 4-4m-4 4-4-4M5 21h14"></path></svg>';
      preview.append(image, download);
      wrapper.append(preview);
      return wrapper;
    }
    if (attachmentBlobType(attachment.fileMime).startsWith("video/")) {
      const preview = document.createElement("div");
      preview.className = "chat-attachment-video-preview grid min-w-0 gap-1";
      const video = document.createElement("video");
      video.className = "chat-attachment-video";
      video.className += compact
        ? " w-full max-h-28 rounded-md border border-border bg-secondary object-contain"
        : " w-full max-h-80 rounded-lg border border-border bg-secondary object-contain";
      video.src = objectUrl;
      video.controls = true;
      video.preload = "metadata";
      video.playsInline = true;
      video.setAttribute("playsinline", "");
      video.setAttribute("aria-label", `Play ${attachment.fileName}`);
      // A codec this browser cannot decode would otherwise leave a dead black
      // box with no way to get at the file, so the player steps aside for the
      // ordinary downloadable card.
      video.addEventListener("error", () => {
        if (!preview.isConnected) return;
        preview.replaceWith(attachmentFileCard(attachment, objectUrl, compact));
      });
      const caption = document.createElement("div");
      caption.className = "flex min-w-0 items-center gap-2";
      const label = document.createElement("span");
      label.className = "min-w-0 flex-1 truncate text-[10px] text-muted-foreground";
      label.textContent = `${attachment.fileName} - ${formatAttachmentSize(attachment.size)}`;
      label.title = attachment.fileName;
      const download = document.createElement("a");
      download.className = "shrink-0 text-[11px] font-semibold text-primary hover:underline";
      download.href = objectUrl;
      download.download = attachment.fileName;
      download.textContent = "Download";
      download.setAttribute("aria-label", `Download ${attachment.fileName}`);
      caption.append(label, download);
      preview.append(video, caption);
      wrapper.append(preview);
      return wrapper;
    }
    wrapper.append(attachmentFileCard(attachment, objectUrl, compact));
    return wrapper;
  }

  function ensureHistoryIndicator() {
    if (!fullLog) return null;
    if (historyIndicator?.isConnected) return historyIndicator;
    historyIndicator = document.createElement("button");
    historyIndicator.type = "button";
    historyIndicator.className = "chat-history-indicator";
    historyIndicator.setAttribute("aria-live", "polite");
    historyIndicator.addEventListener("click", () =>
      revealOlderHistory({ fillBuffer: true }));
    fullLog.prepend(historyIndicator);
    return historyIndicator;
  }

  function currentHistoryRows() {
    const records = historyRowIds
      .map((id) => rows.get(id))
      .filter(Boolean);
    // World-embed history arrives out of order (the Discord bridge folds its
    // own backlog in), so it is sorted here. Both modes read oldest first,
    // newest last.
    return simpleWorldComposer
      ? records.sort((left, right) => left.tsMs - right.tsMs)
      : records;
  }

  function insertHistoryRow(record) {
    if (!fullLog || !record?.el) return;
    const index = historyRowIds.indexOf(record.id);
    for (let offset = index + 1; offset < historyRowIds.length; offset += 1) {
      const next = rows.get(historyRowIds[offset]);
      if (next?.el?.isConnected) {
        fullLog.insertBefore(record.el, next.el);
        return;
      }
    }
    const firstLive = fullLog.querySelector(
      ".chat-message-row:not([data-chat-history])",
    );
    if (firstLive) fullLog.insertBefore(record.el, firstLive);
    else fullLog.append(record.el);
  }

  function materializeFullMessage(record) {
    if (!fullLog || !record || record.el?.isConnected) return record?.el || null;
    clearEmptyState();
    const row = document.createElement("div");
    row.className =
      `chat-message-row chat-message-row--${record.self ? "self" : "peer"} ` +
      "flex items-start gap-3 group px-2 py-1 transition-colors mt-3";
    if (record.history) row.dataset.chatHistory = "";
    const discordLayout = discordChatLayout();
    row.dataset.chatSender = record.senderId || record.who;
    row.dataset.chatTimestamp = String(record.tsMs);
    row.dataset.chatShortTime = fmtChatClock(record.tsMs);
    row.innerHTML = `
      <span class="chat-message-avatar avatar flex h-7 w-7 shrink-0 items-center justify-center overflow-hidden rounded-full border text-base ${record.self ? "border-primary/40 bg-primary/10 text-primary" : "border-border bg-secondary text-foreground"}"></span>
      <div class="chat-message-bubble min-w-0 flex-1">
        <div class="chat-message-head mb-0.5 flex items-baseline gap-2">
          ${authorElementHtml(record, `chat-message-author text-xs font-semibold ${record.self ? "text-primary" : "text-foreground"}`)}${record.external ? "" : userStatusBadgeHtml(record.who)}
          <span class="chat-message-time text-[10px] text-muted-foreground/50 font-mono" title="${escapeHtml(fmtChatFullDate(record.tsMs))}">${escapeHtml(discordLayout ? fmtChatStamp(record.tsMs) : fmtChatTime(record.tsMs))}</span>
        </div>
        <p class="chat-message-text text-sm text-muted-foreground leading-relaxed break-words"></p>
      </div>`;
    bindAuthorProfileCard(row.querySelector(".chat-message-author"));
    const avatarEl = row.querySelector(".chat-message-avatar");
    if (record.external && record.avatarUrl) {
      const image = document.createElement("img");
      image.src = record.avatarUrl;
      image.alt = "";
      image.referrerPolicy = "no-referrer";
      image.className = "h-full w-full rounded-full object-cover";
      avatarEl.replaceChildren(image);
      avatarEl.setAttribute("aria-label", `${record.who} Discord avatar`);
    } else if (record.external) {
      avatarEl.textContent = "D";
      avatarEl.setAttribute("aria-label", `${record.who} Discord avatar`);
    } else {
      hydrateChatAvatar(avatarEl, record.who);
    }
    if (record.sourceLabel) {
      const source = document.createElement("span");
      source.className =
        "chat-message-source rounded-full border border-border px-1.5 py-0.5 " +
        "text-[9px] font-semibold text-muted-foreground";
      source.textContent = record.sourceLabel;
      if (record.sourceLabel === CHANNEL) source.dataset.chatSourceChannel = "";
      row.querySelector(".items-baseline")?.append(source);
    }
    const textEl = row.querySelector("p");
    if (textEl) {
      if (record.text) appendMentionText(textEl, record.text);
      else textEl.remove();
    }
    const content = row.querySelector(".min-w-0.flex-1");
    const renderedAttachment = renderAttachment(record.attachment);
    if (content && renderedAttachment) content.append(renderedAttachment);
    const reactionsEl = document.createElement("div");
    reactionsEl.className = "chat-reactions";
    content?.append(reactionsEl);
    const stickersEl = document.createElement("div");
    stickersEl.className = "chat-stickers";
    row.append(stickersEl);
    const threadSummary = document.createElement("button");
    threadSummary.type = "button";
    threadSummary.className =
      "mt-1 hidden text-[11px] font-semibold text-primary hover:underline";
    threadSummary.addEventListener("click", () => openThread(record));
    content?.append(threadSummary);
    record.el = row;
    record.avatarEl = avatarEl;
    record.textEl = textEl?.parentNode ? textEl : null;
    record.contentEl = content;
    record.reactionsEl = reactionsEl;
    record.stickersEl = stickersEl;
    record.threadSummaryEl = threadSummary;
    if (record.history && !simpleWorldComposer) {
      insertHistoryRow(record);
    } else if (simpleWorldComposer) {
      const next = Array.from(fullLog.querySelectorAll(".chat-message-row"))
        .find((candidate) =>
          Number(candidate.dataset.chatTimestamp || 0) > record.tsMs);
      row.dataset.chatTimestamp = String(record.tsMs);
      if (next) fullLog.insertBefore(row, next);
      else fullLog.append(row);
    } else {
      fullLog.append(row);
    }
    if (record.id && !record.external) {
      content?.append(buildMessageActions(record));
      renderReactions(record.id);
      renderStickers(record.id);
      if (record.editedAt) markEdited(record, record.editedAt);
    }
    renderThreadSummary(record.id);
    return row;
  }

  function fullLogAtNewest() {
    if (!fullLog) return true;
    return fullLog.scrollHeight - fullLog.scrollTop - fullLog.clientHeight <= 48;
  }

  function historyBatchMessages() {
    if (!fullLog) return HISTORY_MIN_BATCH_MESSAGES;
    const viewportRows = Math.ceil(
      Math.max(0, fullLog.clientHeight) / HISTORY_ESTIMATED_ROW_HEIGHT,
    );
    return Math.max(HISTORY_MIN_BATCH_MESSAGES, viewportRows * 2);
  }

  function historyPreloadThreshold() {
    if (!fullLog) return 32;
    return Math.max(32, fullLog.clientHeight * HISTORY_PRELOAD_VIEWPORTS);
  }

  // The newest line lives at the bottom, so "keep up with the room" means
  // keeping the feed scrolled all the way down. The World embed mounts its log
  // inside a closed <details>, so the first render happens at zero height and
  // the scroll only takes once layout lands - hence the follow-up frame and the
  // resize re-pin below.
  function afterNextPaint(callback) {
    requestAnimationFrame(() => setTimeout(callback, 0));
  }

  function nextChatSlice() {
    return new Promise((resolve) => {
      let done = false;
      const finish = () => {
        if (done) return;
        done = true;
        resolve();
      };
      afterNextPaint(finish);
      setTimeout(finish, 100);
    });
  }

  function pinFullLogToNewest() {
    if (!fullLog) return;
    stickToNewest = true;
    if (fullLogPinPending) return;
    fullLogPinPending = true;
    afterNextPaint(() => {
      fullLogPinPending = false;
      if (!stickToNewest || !fullLog) return;
      fullLog.scrollTop = fullLog.scrollHeight;
      lastFullLogScrollTop = fullLog.scrollTop;
      syncChatScrollThumb();
    });
  }

  function scheduleChatScrollThumb() {
    if (chatScrollThumbPending) return;
    chatScrollThumbPending = true;
    afterNextPaint(() => {
      chatScrollThumbPending = false;
      syncChatScrollThumb();
    });
  }

  function renderHistoryWindow({ preserveScroll = false } = {}) {
    if (!fullLog) return;
    const records = currentHistoryRows();
    if (!records.length) {
      historyIndicator?.remove();
      historyIndicator = null;
      ensureEmptyState();
      syncChatScrollThumb();
      return;
    }
    clearEmptyState();
    const previousHeight = fullLog.scrollHeight;
    const visibleCount = Math.min(historyVisibleCount, records.length);
    const hiddenCount = Math.max(0, records.length - visibleCount);
    ensureHistoryIndicator();
    records.forEach((record, index) => {
      const visible = index >= hiddenCount;
      if (visible) materializeFullMessage(record);
      if (record.el) record.el.hidden = !visible;
    });
    const indicator = ensureHistoryIndicator();
    indicator.dataset.complete = hiddenCount ? "false" : "true";
    indicator.disabled = hiddenCount <= 0;
    const nextBatch = Math.min(historyBatchMessages(), hiddenCount);
    indicator.textContent = hiddenCount
      ? `↑ ${hiddenCount} earlier message${hiddenCount === 1 ? "" : "s"} · scroll up to load ${nextBatch}`
      : "Beginning of conversation";
    if (preserveScroll) {
      fullLog.scrollTop += Math.max(0, fullLog.scrollHeight - previousHeight);
      lastFullLogScrollTop = fullLog.scrollTop;
      syncChatScrollThumb();
      return;
    }
    pinFullLogToNewest();
  }

  function revealOlderHistory({ fillBuffer = false } = {}) {
    if (revealingHistory || !fullLog) return;
    revealingHistory = true;
    try {
      let records = currentHistoryRows();
      while (historyVisibleCount < records.length) {
        const previousVisibleCount = historyVisibleCount;
        historyVisibleCount = Math.min(
          records.length,
          historyVisibleCount + historyBatchMessages(),
        );
        renderHistoryWindow({ preserveScroll: true });
        if (
          !fillBuffer ||
          historyVisibleCount === previousVisibleCount ||
          fullLog.scrollTop > historyPreloadThreshold()
        ) {
          break;
        }
        records = currentHistoryRows();
      }
    } finally {
      revealingHistory = false;
    }
  }

  function hasHiddenHistory() {
    return historyVisibleCount < currentHistoryRows().length;
  }

  function scheduleHistoryWheelReset() {
    if (historyWheelResetTimer) clearTimeout(historyWheelResetTimer);
    historyWheelResetTimer = window.setTimeout(() => {
      historyWheelResetTimer = 0;
      historyWheelLatched = false;
    }, 180);
  }

  function appendFullMessage(
    kind,
    who,
    text,
    id,
    senderId,
    tsMs,
    attachment = null,
    deferHistory = false,
    metadata = null,
  ) {
    if (!fullLog) return;
    const self = kind === "self";
    const record = {
      id,
      el: null,
      who,
      tsMs: Number(tsMs) || Date.now(),
      senderId: senderId || "",
      attachment,
      text: text || "",
      self,
      history: Boolean(deferHistory),
      external: Boolean(metadata?.external),
      accountKind: String(metadata?.accountKind || (self ? chatAccountKind() : "")),
      sourceLabel: String(metadata?.sourceLabel || ""),
      avatarUrl: String(metadata?.avatarUrl || ""),
    };
    if (id) rows.set(id, record);
    if (deferHistory && id) {
      historyRowIds.push(id);
      return;
    }
    materializeFullMessage(record);
    if (id) {
      liveRowIds.push(id);
      retireOldLiveRows();
    }
    pinFullLogToNewest();
  }

  function retireOldLiveRows() {
    if (liveRowIds.length <= LIVE_ROW_LIMIT + LIVE_ROW_RETIRE_BATCH) return;
    const retired = liveRowIds.splice(0, liveRowIds.length - LIVE_ROW_LIMIT);
    for (const id of retired) {
      const record = rows.get(id);
      if (!record?.el) continue;
      record.history = true;
      record.el.dataset.chatHistory = "";
      historyRowIds.push(id);
    }
    afterNextPaint(() => renderHistoryWindow({ preserveScroll: !stickToNewest }));
  }

  // ---- message threads ---------------------------------------------------
  // The full /chat client already uses this exact portable envelope:
  //   { type:"thread-reply", rootId, channel, id, senderId, sender, ts, ... }
  // Keep replies out of the top-level timeline and render them in a focused
  // dialog that also works inside World's compact CHAT terminal.

  function threadReplies(rootId) {
    return [...(threadReplyIdsByRoot.get(String(rootId || "")) || [])]
      .map((id) => threadRepliesById.get(id))
      .filter(Boolean)
      .sort((left, right) => left.tsMs - right.tsMs ||
        String(left.id).localeCompare(String(right.id)));
  }

  function renderThreadSummary(rootId) {
    const record = rows.get(String(rootId || ""));
    const summary = record?.threadSummaryEl;
    if (!summary) return;
    const count = threadReplies(rootId).length;
    summary.hidden = count === 0;
    summary.classList.toggle("hidden", count === 0);
    summary.textContent = `${count} ${count === 1 ? "reply" : "replies"}`;
    summary.setAttribute("aria-label", `Open thread with ${summary.textContent}`);
  }

  function threadCard(record, root = false) {
    const card = document.createElement("article");
    card.className =
      "rounded-lg border border-border bg-background/80 px-3 py-2" +
      (root ? " ring-1 ring-primary/20" : "");
    const head = document.createElement("div");
    head.className = "mb-1 flex items-baseline gap-2";
    const profileName = authorProfileName(record);
    const author = document.createElement(profileName ? "a" : "strong");
    author.className = "text-xs text-foreground";
    author.textContent = record.who;
    if (profileName) {
      author.className += " font-semibold hover:underline";
      author.href = mentionProfilePath(profileName);
      author.dataset.chatAuthorProfile = profileName;
      bindAuthorProfileCard(author);
    }
    const time = document.createElement("span");
    time.className = "text-[10px] text-muted-foreground";
    time.textContent = fmtChatTime(record.tsMs);
    head.append(author);
    const authorBadge = window.ForkMeshUserStatus?.badge(
      window.ForkMeshUserStatus.forName(record.who));
    if (authorBadge) head.append(authorBadge);
    head.append(time);
    card.append(head);
    if (record.text) {
      const body = document.createElement("p");
      body.className = "whitespace-pre-wrap break-words text-sm text-foreground";
      appendMentionText(body, record.text);
      card.append(body);
    }
    const attachment = renderAttachment(record.attachment, true);
    if (attachment) card.append(attachment);
    return card;
  }

  function ensureThreadDialog() {
    if (threadDialog?.isConnected) return threadDialog;
    const dialog = document.createElement("dialog");
    dialog.dataset.chatThreadDialog = "true";
    dialog.className =
      "w-[min(94vw,38rem)] max-h-[88vh] overflow-hidden rounded-xl border border-border " +
      "bg-card p-0 text-foreground shadow-2xl backdrop:bg-black/60";
    dialog.innerHTML = `
      <form method="dialog" class="fm-dialog-header">
        <div class="fm-dialog-title">Thread <span data-chat-thread-count class="ml-1 text-xs font-normal text-muted-foreground"></span></div>
        <button type="submit" class="fm-btn fm-btn-invisible fm-btn-icon fm-btn-sm" aria-label="Close thread"><svg aria-hidden="true" width="16" height="16" viewBox="0 0 16 16" fill="currentColor"><path d="M3.72 3.72a.75.75 0 0 1 1.06 0L8 6.94l3.22-3.22a.749.749 0 0 1 1.275.326.749.749 0 0 1-.215.734L9.06 8l3.22 3.22a.749.749 0 0 1-.326 1.275.749.749 0 0 1-.734-.215L8 9.06l-3.22 3.22a.751.751 0 0 1-1.042-.018.751.751 0 0 1-.018-1.042L6.94 8 3.72 4.78a.75.75 0 0 1 0-1.06Z"></path></svg></button>
      </form>
      <div data-chat-thread-list class="grid max-h-[62vh] gap-2 overflow-y-auto p-4"></div>
      <div class="fm-dialog-footer items-end">
        <textarea data-chat-thread-input rows="2" maxlength="16000" class="min-w-0 flex-1 resize-y rounded-md border border-border bg-background px-3 py-2 text-sm focus:border-primary focus:outline-none" placeholder="Reply in thread…" aria-label="Reply in thread"></textarea>
        <button data-chat-thread-send type="button" class="inline-flex h-7 shrink-0 items-center rounded-md bg-primary px-2.5 text-xs font-medium text-primary-foreground hover:bg-primary/90">Reply</button>
      </div>`;
    dialog.querySelector("[data-chat-thread-send]")?.addEventListener(
      "click", sendThreadReply,
    );
    dialog.querySelector("[data-chat-thread-input]")?.addEventListener(
      "keydown", (event) => {
        if (event.key === "Enter" && !event.shiftKey) {
          event.preventDefault();
          sendThreadReply();
        }
      },
    );
    dialog.addEventListener("close", () => {
      activeThreadRootId = "";
    });
    document.body.append(dialog);
    threadDialog = dialog;
    return dialog;
  }

  function renderThreadDialog() {
    if (!activeThreadRootId || !threadDialog) return;
    const list = threadDialog.querySelector("[data-chat-thread-list]");
    const count = threadDialog.querySelector("[data-chat-thread-count]");
    if (!list) return;
    list.textContent = "";
    const root = rows.get(activeThreadRootId);
    if (root) list.append(threadCard(root, true));
    else {
      const missing = document.createElement("p");
      missing.className = "text-sm text-muted-foreground";
      missing.textContent = "The root message is no longer available.";
      list.append(missing);
    }
    const replies = threadReplies(activeThreadRootId);
    for (const reply of replies) list.append(threadCard(reply));
    if (count) {
      count.textContent = `${replies.length} ${replies.length === 1 ? "reply" : "replies"}`;
    }
    list.scrollTop = list.scrollHeight;
  }

  function openThread(record) {
    const rootId = String(record?.rootId || record?.id || "").slice(0, 96);
    if (!rootId) return;
    activeThreadRootId = rootId;
    const dialog = ensureThreadDialog();
    renderThreadDialog();
    if (!dialog.open) dialog.showModal();
    dialog.querySelector("[data-chat-thread-input]")?.focus();
  }

  async function sendThreadReply() {
    if (!activeThreadRootId || !canJoinChat() || !threadDialog) return;
    const input = threadDialog.querySelector("[data-chat-thread-input]");
    const moderation = await moderateDashboardText(
      String(input?.value || "").trim().slice(0, MAX_TEXT));
    if (!moderation) {
      setStatus("Chat filter unavailable · message not sent");
      return;
    }
    const text = moderation.text;
    if (!text) return;
    const plain = makePlain("thread-reply", {
      rootId: activeThreadRootId,
      channel: CHANNEL,
      text,
    });
    if (input) input.value = "";
    renderThreadEntry(plain, "self", true);
    runWhenConnected(() => send(plain));
    if (moderation.changed) appendSystem("Some words were filtered.");
  }

  function renderThreadEntry(entry, kind = "peer", live = false) {
    if (!entry || !allowedChatAccountKind(entry.accountKind)) return;
    if (kind !== "self" && !frameAuth?.isSealedPlain(entry)) return;
    entry = normalizedPublicWorldFrame(entry);
    const rootId = String(entry.rootId || "").slice(0, 96);
    if (!rootId || entry.channel !== CHANNEL || !once(entry.id)) return;
    const record = {
      id: entry.id,
      rootId,
      who: entry.botVia ? String(entry.sender) : String(entry.sender || "peer").slice(0, MAX_NAME),
      senderId: String(entry.senderId || ""),
      tsMs: Number(entry.ts) || Date.now(),
      text: String(entry.text || "").slice(0, MAX_TEXT),
      attachment: attachmentFromEntry(entry),
      self: entry.senderId === selfId || kind === "self",
      accountKind: String(entry.accountKind || ""),
    };
    threadRepliesById.set(record.id, record);
    const ids = threadReplyIdsByRoot.get(rootId) || new Set();
    ids.add(record.id);
    threadReplyIdsByRoot.set(rootId, ids);
    renderThreadSummary(rootId);
    if (activeThreadRootId === rootId) renderThreadDialog();
    if (live && entry.senderId !== selfId) {
      rememberMentionPerson(record.who, Date.now());
    }
  }

  // ---- edit / delete own messages ----------------------------------------
  // The relay already replays "edit"/"delete" frames (handlePlain below, and
  // the desktop client's kDurableTypes), so the dashboard only needs the
  // author-side controls. Both frames are honoured by peers only when the
  // sender id matches the original message's, so the same guard is applied
  // here before anything is broadcast.

  function messageActionButton(label, onClick, { danger = false, ariaLabel } = {}) {
    const button = document.createElement("button");
    button.type = "button";
    button.className =
      (danger ? "fm-btn fm-btn-sm fm-btn-danger text-destructive" : "fm-btn fm-btn-invisible fm-btn-sm");
    button.textContent = label;
    button.setAttribute("aria-label", ariaLabel || `${label} message`);
    button.addEventListener("click", onClick);
    return button;
  }

  function buildMessageActions(record) {
    const actions = document.createElement("div");
    // .chat-message-actions is revealed on row hover / keyboard focus by the
    // dashboard shell stylesheet (Tailwind's group-hover variant is not in the
    // pre-built dashboard/tailwind.css, so the reveal is hand-written CSS).
    actions.className = "chat-message-actions ml-auto flex shrink-0 items-center gap-1";
    actions.append(
      messageActionButton("Reply", () => openThread(record), {
        ariaLabel: "Reply in thread",
      }),
    );
    actions.append(
      messageActionButton("☺", (event) =>
        showReactionPicker(record, event.currentTarget), {
        ariaLabel: "Add reaction",
      }),
    );
    actions.append(
      messageActionButton("Sticker", (event) =>
        showStickerPicker(record, event.currentTarget), {
        ariaLabel: "Add sticker",
      }),
    );
    // Anyone can turn any message into a repository issue — the useful case is
    // filing someone else's bug report, so this is deliberately not author-only
    // (the issue itself is signed by, and attributed to, whoever files it).
    if (record.text) {
      actions.append(
        messageActionButton("Issue", () => void beginIssueFromMessage(record), {
          ariaLabel: "Create an issue from this message",
        }),
      );
    }
    if (record.self && record.senderId === selfId) {
      if (record.text) {
        actions.append(messageActionButton("Edit", () => beginMessageEdit(record)));
      }
      actions.append(
        messageActionButton("Delete", () => requestMessageDelete(record), { danger: true })
      );
    }
    record.actionsEl = actions;
    return actions;
  }

  let activeReactionPicker = null;

  function reactionKey(plain) {
    return String(plain.reactorId || plain.senderId || "");
  }

  function applyReaction(plain) {
    const sticker = STICKERS_BY_SLUG.get(String(plain.sticker || ""));
    if (sticker) {
      applySticker(plain, sticker);
      return;
    }
    const target = String(plain.target || "");
    const emoji = String(plain.emoji || "").slice(0, 8);
    const reactor = reactionKey(plain);
    if (!target || !emoji || !reactor) return;
    const perMessage = reactions.get(target) || new Map();
    reactions.set(target, perMessage);
    const perEmoji = perMessage.get(emoji) || new Map();
    perMessage.set(emoji, perEmoji);
    if (plain.added) {
      perEmoji.set(
        reactor,
        String((frameAuth?.frameAuth(plain) ? plain.sender : plain.reactorName || plain.sender) || "peer").slice(0, MAX_NAME),
      );
    } else {
      perEmoji.delete(reactor);
    }
    if (!perEmoji.size) perMessage.delete(emoji);
    if (!perMessage.size) reactions.delete(target);
    renderReactions(target);
  }

  function renderReactions(messageId) {
    const record = rows.get(messageId);
    if (!record?.reactionsEl) return;
    record.reactionsEl.textContent = "";
    const perMessage = reactions.get(messageId);
    if (!perMessage) return;
    for (const [emoji, reactors] of perMessage) {
      if (!reactors.size) continue;
      const chip = document.createElement("button");
      chip.type = "button";
      chip.className =
        "chat-reaction-chip" + (reactors.has(selfId) ? " is-mine" : "");
      chip.title = [...reactors.values()].join(", ");
      chip.setAttribute(
        "aria-label",
        `${emoji} reaction from ${reactors.size} ${
          reactors.size === 1 ? "person" : "people"
        }`,
      );
      const face = document.createElement("span");
      face.textContent = emoji;
      const count = document.createElement("span");
      count.className = "chat-reaction-count";
      count.textContent = String(reactors.size);
      chip.append(face, count);
      chip.addEventListener("click", () => toggleReaction(messageId, emoji));
      record.reactionsEl.append(chip);
    }
    if (discordChatLayout() && record.reactionsEl.childElementCount) {
      const add = document.createElement("button");
      add.type = "button";
      add.className = "chat-reaction-chip";
      add.dataset.chatReactionAdd = "";
      add.textContent = "☺";
      add.setAttribute("aria-label", "Add another reaction");
      add.addEventListener("click", (event) =>
        showReactionPicker(record, event.currentTarget));
      record.reactionsEl.append(add);
    }
  }

  function toggleReaction(messageId, emoji) {
    if (!canJoinChat()) {
      showUserOnlyState();
      return;
    }
    const mine = Boolean(reactions.get(messageId)?.get(emoji)?.has(selfId));
    const plain = makePlain("reaction", {
      conversation: CHANNEL_LABEL,
      target: messageId,
      emoji,
      reactorId: selfId,
      reactorName: displayName(),
      added: !mine,
    });
    seen.add(plain.id);
    applyReaction(plain);
    runWhenConnected(() => send(plain));
  }

  function applySticker(plain, sticker) {
    const target = String(plain.target || "");
    const reactor = reactionKey(plain);
    if (!target || !reactor) return;
    const perMessage = stickers.get(target) || new Map();
    stickers.set(target, perMessage);
    const placers = perMessage.get(sticker.slug) || new Map();
    perMessage.set(sticker.slug, placers);
    if (plain.added) {
      placers.set(
        reactor,
        String((frameAuth?.frameAuth(plain) ? plain.sender : plain.reactorName || plain.sender) || "peer").slice(0, MAX_NAME),
      );
    } else {
      placers.delete(reactor);
    }
    if (!placers.size) perMessage.delete(sticker.slug);
    if (!perMessage.size) stickers.delete(target);
    renderStickers(target);
  }

  function stickerBadge(sticker) {
    const badge = document.createElement("button");
    badge.type = "button";
    badge.className = "chat-sticker";
    badge.dataset.sticker = sticker.slug;
    badge.style.background = sticker.color;
    badge.style.setProperty("--chat-sticker-tilt", `${sticker.tilt}deg`);
    const label = document.createElement("span");
    label.textContent = sticker.text;
    badge.append(label);
    return badge;
  }

  function renderStickers(messageId) {
    const record = rows.get(messageId);
    if (!record?.stickersEl) return;
    record.stickersEl.textContent = "";
    const perMessage = record.external ? null : stickers.get(messageId);
    record.el?.classList.toggle("has-stickers", Boolean(perMessage?.size));
    if (!perMessage) return;
    for (const sticker of STICKER_CATALOG) {
      const placers = perMessage.get(sticker.slug);
      if (!placers?.size) continue;
      const badge = stickerBadge(sticker);
      const mine = placers.has(selfId);
      badge.classList.toggle("is-mine", mine);
      badge.setAttribute("aria-pressed", mine ? "true" : "false");
      const names = [...placers.values()].join(", ");
      badge.title = `${sticker.text} from ${names}`;
      badge.setAttribute("aria-label", `${sticker.text} sticker from ${names}`);
      if (placers.size > 1) {
        const count = document.createElement("span");
        count.className = "chat-sticker-count";
        count.textContent = `\u00d7${placers.size}`;
        badge.append(count);
      }
      badge.addEventListener("click", () => toggleSticker(messageId, sticker.slug));
      record.stickersEl.append(badge);
    }
  }

  function toggleSticker(messageId, slug) {
    const sticker = STICKERS_BY_SLUG.get(slug);
    if (!sticker || rows.get(messageId)?.external) return;
    if (!canJoinChat()) {
      showUserOnlyState();
      return;
    }
    const mine = Boolean(stickers.get(messageId)?.get(slug)?.has(selfId));
    const plain = makePlain("reaction", {
      conversation: CHANNEL_LABEL,
      target: messageId,
      emoji: sticker.emoji,
      sticker: sticker.slug,
      reactorId: selfId,
      reactorName: displayName(),
      added: !mine,
    });
    seen.add(plain.id);
    applyReaction(plain);
    runWhenConnected(() => send(plain));
  }

  function showStickerPicker(record, trigger) {
    if (!record?.id || !record.contentEl || record.external) return;
    closeReactionPicker();
    const picker = document.createElement("div");
    picker.className = "chat-sticker-picker";
    picker.setAttribute("role", "toolbar");
    picker.setAttribute("aria-label", "Choose a sticker");
    const placed = stickers.get(record.id);
    STICKER_CATALOG.forEach((sticker) => {
      const button = stickerBadge(sticker);
      const mine = Boolean(placed?.get(sticker.slug)?.has(selfId));
      button.classList.toggle("is-mine", mine);
      button.setAttribute("aria-pressed", mine ? "true" : "false");
      button.setAttribute("aria-label", `Stick ${sticker.text}`);
      button.addEventListener("click", () => {
        toggleSticker(record.id, sticker.slug);
        closeReactionPicker();
      });
      picker.append(button);
    });
    picker.addEventListener("keydown", (event) => {
      if (event.key !== "Escape") return;
      event.preventDefault();
      closeReactionPicker({ restoreFocus: true });
    });
    record.contentEl.append(picker);
    activeReactionPicker = { element: picker, trigger };
    picker.querySelector("button")?.focus();
  }

  function closeReactionPicker({ restoreFocus = false } = {}) {
    if (!activeReactionPicker) return;
    const { element, trigger } = activeReactionPicker;
    element.remove();
    activeReactionPicker = null;
    if (restoreFocus) trigger?.focus();
  }

  function showReactionPicker(record, trigger) {
    if (!record?.id || !record.contentEl) return;
    closeReactionPicker();
    const picker = document.createElement("div");
    picker.className = "chat-reaction-picker";
    picker.setAttribute("role", "toolbar");
    picker.setAttribute("aria-label", "Choose a reaction");
    REACTION_EMOJI.forEach((emoji) => {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = emoji;
      button.setAttribute("aria-label", `React with ${emoji}`);
      button.addEventListener("click", () => {
        toggleReaction(record.id, emoji);
        closeReactionPicker();
      });
      picker.append(button);
    });
    picker.addEventListener("keydown", (event) => {
      if (event.key !== "Escape") return;
      event.preventDefault();
      closeReactionPicker({ restoreFocus: true });
    });
    record.contentEl.append(picker);
    activeReactionPicker = { element: picker, trigger };
    picker.querySelector("button")?.focus();
  }

  // An "(edited)" marker so a rewritten message never silently replaces what
  // peers already read (matches the full chat page).
  function markEdited(record, editedAt) {
    if (!record?.textEl) return;
    const stamp = Number(editedAt) || Date.now();
    let marker = record.textEl.querySelector(".chat-edited");
    if (!marker) {
      marker = document.createElement("span");
      marker.className = "chat-edited ml-1 text-[10px] text-muted-foreground/50";
      marker.textContent = "(edited)";
      record.textEl.append(marker);
    }
    marker.title = `Edited ${new Date(stamp).toLocaleString()}`;
  }

  // The action rows carry Tailwind's `flex` utility, which outranks the
  // [hidden] preflight rule in the pre-built stylesheet — toggle display
  // directly rather than the attribute.
  function showElement(el, visible) {
    if (el) el.style.display = visible ? "" : "none";
  }

  function closeMessageEditor(record, { restoreActions = true } = {}) {
    record.editorEl?.remove();
    record.editorEl = null;
    showElement(record.textEl, true);
    if (restoreActions) showElement(record.actionsEl, true);
  }

  function beginMessageEdit(record) {
    if (!record?.textEl || record.senderId !== selfId) return;
    if (record.editorEl) {
      record.editorEl.querySelector("textarea")?.focus();
      return;
    }
    showElement(record.textEl, false);
    showElement(record.actionsEl, false);
    const editor = document.createElement("div");
    editor.className = "chat-inline-editor mt-1 flex flex-col gap-1.5";
    const textarea = document.createElement("textarea");
    textarea.className =
      "w-full resize-y rounded-md border border-border bg-background px-2 py-1 text-sm";
    textarea.rows = 2;
    textarea.maxLength = MAX_TEXT;
    textarea.value = record.text || "";
    textarea.setAttribute("aria-label", "Edit message");
    const controls = document.createElement("div");
    controls.className = "flex items-center gap-2";
    const cancel = messageActionButton(
      "Cancel", () => closeMessageEditor(record), { ariaLabel: "Cancel edit" });
    const save = messageActionButton(
      "Save", () => saveMessageEdit(record, textarea.value), { ariaLabel: "Save edit" });
    textarea.addEventListener("keydown", (event) => {
      if (event.key === "Escape") {
        event.preventDefault();
        closeMessageEditor(record);
      } else if (event.key === "Enter" && !event.shiftKey) {
        event.preventDefault();
        saveMessageEdit(record, textarea.value);
      }
    });
    controls.append(cancel, save);
    editor.append(textarea, controls);
    record.editorEl = editor;
    (record.contentEl || record.el).append(editor);
    textarea.focus();
    textarea.setSelectionRange(textarea.value.length, textarea.value.length);
  }

  async function saveMessageEdit(record, source) {
    if (record.senderId !== selfId) return;
    const moderation = await moderateDashboardText(
      String(source || "").trim().slice(0, MAX_TEXT));
    if (!moderation) {
      setStatus("Chat filter unavailable · edit not sent");
      return;
    }
    const text = moderation.text;
    if (!text) return;
    const editedAt = Date.now();
    const plain = makePlain("edit", {
      conversation: CHANNEL_LABEL,
      target: record.id,
      text,
      editedAt,
    });
    record.text = text;
    if (record.rootId) {
      record.editedAt = editedAt;
      renderThreadDialog();
    } else {
      renderMessageText(record.textEl, text);
      markEdited(record, editedAt);
      closeMessageEditor(record);
    }
    const sideEntry = sideEntries.find((entry) => entry.id === record.id);
    if (sideEntry) {
      sideEntry.text = text;
      renderSideMessages();
    }
    seen.add(plain.id);
    runWhenConnected(() => send(plain));
    if (moderation.changed) appendSystem("Some words were filtered.");
  }

  // Deleting is irreversible for every reader, so ask first — inline, because
  // this chat also runs inside the World's same-origin embed where a blocking
  // window.confirm() would freeze the host frame.
  function requestMessageDelete(record) {
    if (record.senderId !== selfId || !record.actionsEl) return;
    if (record.confirmEl) return;
    showElement(record.actionsEl, false);
    const confirmBar = document.createElement("div");
    confirmBar.className =
      "chat-delete-confirm ml-auto flex shrink-0 items-center gap-1 text-[10px] text-muted-foreground";
    const label = document.createElement("span");
    label.textContent = "Delete?";
    const cancel = messageActionButton("Cancel", () => {
      confirmBar.remove();
      record.confirmEl = null;
      showElement(record.actionsEl, true);
    }, { ariaLabel: "Cancel delete" });
    const confirm = messageActionButton(
      "Delete", () => confirmMessageDelete(record), { danger: true, ariaLabel: "Confirm delete" });
    confirmBar.append(label, cancel, confirm);
    record.confirmEl = confirmBar;
    record.el.append(confirmBar);
    confirm.focus();
  }

  function confirmMessageDelete(record) {
    if (record.senderId !== selfId) return;
    const plain = makePlain("delete", {
      conversation: CHANNEL_LABEL,
      target: record.id,
    });
    record.confirmEl = null;
    seen.add(plain.id);
    removeMessage(record.id);
    runWhenConnected(() => send(plain));
  }

  // ---- convert a message into a repository issue --------------------------
  // A bug report typed into chat should not have to be retyped on the issues
  // page, so every message carries an "Issue" control that opens an inline
  // form: an editable title seeded from the first line, and the repository
  // picker mirrored from the composer. The whole message becomes the body with
  // an attribution line, and it is filed through the same signed "open" event
  // the dashboard's new-issue form posts (fileWebIssue).

  function closeIssueForm(record) {
    record.issueFormEl?.remove();
    record.issueFormEl = null;
    showElement(record.actionsEl, true);
  }

  async function beginIssueFromMessage(record) {
    if (!record?.text) return;
    if (record.issueFormEl) {
      record.issueFormEl.querySelector("input")?.focus();
      return;
    }
    const filing = await issueFiling();
    if (record.issueFormEl) return; // a second click won while the module loaded
    showElement(record.actionsEl, false);
    const form = document.createElement("div");
    form.className = "chat-issue-form mt-1 flex flex-col gap-1.5";
    const title = document.createElement("input");
    title.type = "text";
    title.className =
      "w-full rounded-md border border-border bg-background px-2 py-1 text-sm";
    title.maxLength = 200;
    title.value = filing.issueTitleFromMessage(record.text);
    title.setAttribute("aria-label", "Issue title");
    // The composer's picker is the only repository list this bundle loads
    // (/api/repositories), so the form clones it instead of fetching again.
    const repository = document.createElement("select");
    repository.className =
      "w-full rounded-md border border-border bg-background px-2 py-1 text-xs";
    repository.setAttribute("aria-label", "Issue repository");
    for (const option of fullRepository?.options || []) {
      repository.append(option.cloneNode(true));
    }
    repository.value = String(fullRepository?.value || "");
    const status = document.createElement("span");
    status.className = "text-[10px] text-muted-foreground";
    status.setAttribute("role", "status");
    const setStatus = (message, bad = false) => {
      status.textContent = message;
      status.className = bad
        ? "text-[10px] text-destructive"
        : "text-[10px] text-muted-foreground";
    };
    const controls = document.createElement("div");
    controls.className = "flex items-center gap-2";
    const cancel = messageActionButton("Cancel", () => closeIssueForm(record), {
      ariaLabel: "Cancel issue",
    });
    const create = messageActionButton(
      "Create issue",
      () => void createIssueFromMessage(
        record, filing, title, repository, setStatus, create),
      { ariaLabel: "File this issue" },
    );
    title.addEventListener("keydown", (event) => {
      if (event.key === "Escape") {
        event.preventDefault();
        closeIssueForm(record);
      } else if (event.key === "Enter") {
        event.preventDefault();
        void createIssueFromMessage(
          record, filing, title, repository, setStatus, create);
      }
    });
    controls.append(cancel, create);
    form.append(title, repository, controls, status);
    record.issueFormEl = form;
    (record.contentEl || record.el).append(form);
    if (!repository.options.length) {
      setStatus("Sign in and pick a repository to file issues.", true);
    }
    title.focus();
    title.select();
  }

  async function createIssueFromMessage(
    record, filing, titleEl, repositoryEl, setStatus, createEl) {
    const repository = repositoryFromSelect(repositoryEl);
    if (!repository) {
      setStatus("Choose a repository for this issue.", true);
      repositoryEl.focus();
      return;
    }
    const title = String(titleEl.value || "").trim().slice(0, 200);
    if (!title) {
      setStatus("An issue needs a title.", true);
      titleEl.focus();
      return;
    }
    createEl.disabled = true;
    setStatus("Signing issue…");
    try {
      const filed = await fileWebIssue(
        repository,
        title,
        filing.issueBodyFromMessage({
          text: record.text,
          who: record.who,
          tsMs: record.tsMs,
          channelLabel: CHANNEL_LABEL,
        }),
      );
      closeIssueForm(record);
      appendSystem(filing.issueFiledSummary(
        `${repository.owner}/${repository.name}`, title, filed));
    } catch (error) {
      createEl.disabled = false;
      setStatus(String(error?.message || "The issue could not be filed."), true);
    }
  }

  // The rail's mini chat mirrors the full view at a smaller scale: avatar +
  // name + time header with the message below, ordered by each message's own
  // timestamp so replayed history and live traffic interleave correctly with
  // the newest at the bottom.
  function renderSideMessages() {
    if (!sideLog || sideRenderPending) return;
    sideRenderPending = true;
    requestAnimationFrame(() => {
      sideRenderPending = false;
      renderSideMessagesNow();
    });
  }

  function renderSideMessagesNow() {
    if (!sideLog) return;
    if (!sideEntries.length) {
      sideLog.innerHTML = sideEmptyHtml();
      return;
    }
    sideLog.textContent = "";
    for (const message of sideEntries.slice(-MAX_SIDE_MESSAGES)) {
      const self = message.kind === "self";
      const row = document.createElement("div");
      row.className = "flex items-start gap-2 px-1 py-1 rounded-md hover:bg-secondary/40 transition-colors mt-2";
      row.innerHTML = `
        <span class="chat-message-avatar avatar flex h-6 w-6 shrink-0 items-center justify-center overflow-hidden rounded-full border text-sm ${self ? "border-primary/40 bg-primary/10 text-primary" : "border-border bg-secondary text-foreground"}"></span>
        <div class="min-w-0 flex-1">
          <div class="flex items-baseline gap-1.5">
            ${authorElementHtml(message, `chat-side-message-author text-[11px] font-semibold ${self ? "text-primary" : "text-foreground"}`, false)}${message.external ? "" : userStatusBadgeHtml(message.who)}
            <span class="text-[9px] text-muted-foreground/50 font-mono">${escapeHtml(fmtChatTime(message.tsMs))}</span>
          </div>
          <p class="text-xs text-muted-foreground leading-relaxed break-words"></p>
        </div>`;
      bindAuthorProfileCard(row.querySelector(".chat-side-message-author"));
      const avatar = row.querySelector(".chat-message-avatar");
      if (message.external) {
        avatar.textContent = "D";
        avatar.setAttribute("aria-label", "Discord");
      } else {
        hydrateChatAvatar(row.querySelector(".chat-message-avatar"), message.who);
      }
      const textEl = row.querySelector("p");
      if (textEl) {
        if (message.text) appendMentionText(textEl, message.text);
        else textEl.remove();
      }
      const content = row.querySelector(".min-w-0.flex-1");
      const renderedAttachment = renderAttachment(message.attachment, true);
      if (content && renderedAttachment) content.append(renderedAttachment);
      sideLog.append(row);
    }
    const bottom = document.createElement("div");
    bottom.dataset.chatBottom = "";
    sideLog.append(bottom);
    afterNextPaint(() => {
      sideLog.scrollTo({ top: sideLog.scrollHeight, behavior: "smooth" });
    });
  }

  function appendSideMessage(
    kind,
    who,
    text,
    id,
    senderId,
    tsMs,
    attachment = null,
    deferRender = false,
    metadata = null,
  ) {
    // Insert in timestamp order (append is the common case) so the newest
    // message is always the bottom row even when retained history replays
    // after live messages have already landed.
    const entry = {
      kind,
      who,
      text,
      id,
      senderId,
      tsMs: Number(tsMs) || Date.now(),
      attachment,
      external: Boolean(metadata?.external),
      accountKind: String(metadata?.accountKind || (kind === "self" ? chatAccountKind() : "")),
      sourceLabel: String(metadata?.sourceLabel || ""),
    };
    let index = sideEntries.length;
    while (index > 0 && Number(sideEntries[index - 1].tsMs) > entry.tsMs) index -= 1;
    sideEntries.splice(index, 0, entry);
    if (!deferRender) renderSideMessages();
  }

  function appendMessage(
    kind,
    who,
    text,
    id,
    senderId,
    tsMs,
    attachment = null,
    deferHistory = false,
    accountKind = "",
  ) {
    if (orgAgentIdentity(who, senderId) && !orgAgentConversationAccess) return;
    const channelMetadata = {
      sourceLabel: CHANNEL.startsWith("#") ? CHANNEL : `#${CHANNEL}`,
      accountKind,
    };
    appendFullMessage(
      kind,
      who,
      text,
      id,
      senderId,
      tsMs,
      attachment,
      deferHistory,
      channelMetadata,
    );
    appendSideMessage(
      kind,
      who,
      text,
      id,
      senderId,
      tsMs,
      attachment,
      deferHistory,
      channelMetadata,
    );
    rememberContext(who, text);
  }

  function appendDiscordMessage(message) {
    const organization = String(message?.organization || "").trim();
    const channelId = String(message?.channelId || "").trim();
    const channelName = String(message?.channelName || message?.channelId || "")
      .trim();
    const providerId = String(message?.id || "").trim();
    if (!organization || !channelId || !channelName || !providerId) return false;
    const id = `discord:${organization}:${channelId}:${providerId}`;
    if (rows.has(id)) return false;
    const projectedName = String(message?.author?.name || "").trim();
    const who = projectedName && !/^\d{5,}$/.test(projectedName)
      ? projectedName
      : "Discord member";
    const text = String(message?.content || "").trim();
    if (!text) return false;
    const parsedTime = Date.parse(String(message?.createdAt || ""));
    const tsMs = Number.isFinite(parsedTime) ? parsedTime : Date.now();
    const metadata = {
      external: true,
      sourceLabel: `Discord · ${organization} · #${channelName}`,
      avatarUrl: /^https:\/\/cdn\.discordapp\.com\/(?:avatars\/\d{17,20}\/[A-Za-z0-9_-]{2,128}\.(?:png|webp)(?:\?size=64)?|embed\/avatars\/[0-5]\.png)$/.test(
        String(message?.author?.avatarUrl || ""),
      )
        ? String(message.author.avatarUrl)
        : "",
    };
    appendFullMessage(
      "peer", who, text, id, `discord:${providerId}`, tsMs, null, false, metadata,
    );
    appendSideMessage(
      "peer", who, text, id, `discord:${providerId}`, tsMs, null, false, metadata,
    );
    // Provider text is display-only. It never enters ForkBot's prompt context
    // unless a person explicitly quotes it into a ForkMesh message.
    return true;
  }

  function appendSystem(text) {
    if (fullLog) {
      clearEmptyState();
      const row = document.createElement("div");
      row.className = "chat-system-bubble my-2 rounded-md border border-border px-3 py-2 text-xs text-muted-foreground";
      row.textContent = text;
      fullLog.append(row);
      pinFullLogToNewest();
    }
  }

  function removeMessage(id, { deferRender = false } = {}) {
    const threadReply = threadRepliesById.get(id);
    if (threadReply) {
      threadRepliesById.delete(id);
      const ids = threadReplyIdsByRoot.get(threadReply.rootId);
      ids?.delete(id);
      if (ids && !ids.size) threadReplyIdsByRoot.delete(threadReply.rootId);
      if (threadReply.attachment) releaseAttachment(threadReply.attachment);
      reactions.delete(id);
      stickers.delete(id);
      renderThreadSummary(threadReply.rootId);
      if (activeThreadRootId === threadReply.rootId) renderThreadDialog();
      return;
    }
    const rec = rows.get(id);
    if (activeReactionPicker?.element && rec?.el?.contains(
        activeReactionPicker.element)) {
      closeReactionPicker();
    }
    if (rec?.el?.parentNode) rec.el.parentNode.removeChild(rec.el);
    if (rec?.attachment) releaseAttachment(rec.attachment);
    rows.delete(id);
    const idx = sideEntries.findIndex((entry) => entry.id === id);
    if (idx >= 0 && sideEntries[idx].attachment !== rec?.attachment) {
      releaseAttachment(sideEntries[idx].attachment);
    }
    if (idx >= 0) sideEntries.splice(idx, 1);
    const historyIndex = historyRowIds.indexOf(id);
    if (historyIndex >= 0) historyRowIds.splice(historyIndex, 1);
    const liveIndex = liveRowIds.indexOf(id);
    if (liveIndex >= 0) liveRowIds.splice(liveIndex, 1);
    reactions.delete(id);
    stickers.delete(id);
    if (!deferRender) {
      renderSideMessages();
      if (historyIndex >= 0) {
        renderHistoryWindow({ preserveScroll: true });
      }
    }
  }

  function makePlain(type, extra) {
    return Object.assign(
      {
        type,
        id:
          (crypto.randomUUID && crypto.randomUUID()) ||
          `${Math.random()}`.slice(2) + Date.now(),
        senderId: selfId,
        sender: displayName(),
        accountKind: chatAccountKind(),
        ts: Date.now(),
      },
      ownUserStatusFrameField(type),
      extra || {}
    );
  }

  // Away + custom status (user-status.js; absent inside the World embed) ride
  // hello/presence beats, the same field the /chat page and apps send.
  function ownUserStatusFrameField(type) {
    if ((type !== "hello" && type !== "presence") || !userSession()) return {};
    const own = window.ForkMeshUserStatus?.ownFrameStatus();
    return own ? { userStatus: own } : {};
  }

  // The status emoji beside an author name, as HTML for the row templates.
  function userStatusBadgeHtml(name) {
    const badge = window.ForkMeshUserStatus?.badge(
      window.ForkMeshUserStatus.forName(name));
    return badge ? badge.outerHTML : "";
  }

  function once(id) {
    if (!id || seen.has(id)) return false;
    seen.add(id);
    return true;
  }

  // Inside the World embed, mirror each live chat line to the parent page so
  // it can float the message above the speaker's avatar and fade it out
  // (world.js handleWorldChatMessage). Same-origin only. History replays are
  // marked so the parent updates only its collapsed CHAT bar with the most
  // recent line — reconnects never resurrect old bubbles.
  const WORLD_EMBED_BUBBLES =
    requestedParams.get("worldEmbed") === "1" ||
    Boolean(document.querySelector("[data-world-native-chat]"));
  const worldAttachmentPreviewCache = new WeakMap();

  function worldAttachmentPreview(attachment) {
    if (
      !attachment ||
      typeof attachment !== "object" ||
      !String(attachment.fileMime || "").startsWith("image/")
    ) {
      return Promise.resolve("");
    }
    if (worldAttachmentPreviewCache.has(attachment)) {
      return worldAttachmentPreviewCache.get(attachment);
    }
    const pending = new Promise((resolve) => {
      const image = new Image();
      image.onload = () => {
        try {
          const scale = Math.min(1, 160 / Math.max(image.width, image.height));
          const canvas = document.createElement("canvas");
          canvas.width = Math.max(1, Math.round(image.width * scale));
          canvas.height = Math.max(1, Math.round(image.height * scale));
          canvas.getContext("2d")?.drawImage(
            image,
            0,
            0,
            canvas.width,
            canvas.height,
          );
          const preview = canvas.toDataURL("image/webp", 0.7);
          resolve(preview.length <= 120_000 ? preview : "");
        } catch (_) {
          resolve("");
        }
      };
      image.onerror = () => resolve("");
      try {
        image.src = attachmentObjectUrl(attachment);
      } catch (_) {
        resolve("");
      }
    });
    worldAttachmentPreviewCache.set(attachment, pending);
    return pending;
  }

  // Authored by the signed-in account, but not necessarily by this browser —
  // another tab, a phone, or the desktop client counts too. Kept apart from
  // `self` (a strict senderId match) because only `self` may raise a bubble
  // over the local avatar; a display name alone is not proof of identity.
  // The World uses this to keep your own lines out of its unread count.
  function isOwnChatLine(sender, senderId) {
    if (senderId === selfId) return true;
    const account = String(userSession()?.nodeName || "").trim().toLowerCase();
    if (!account) return false;
    return String(sender || "").trim().toLowerCase() === account;
  }

  function emitWorldChatBubble(
    sender,
    senderId,
    text,
    history = false,
    meta = {},
  ) {
    if (!WORLD_EMBED_BUBBLES) return;
    if (orgAgentIdentity(sender, senderId) && !orgAgentConversationAccess) return;
    const line = String(text || "").trim().slice(0, 200);
    const attachmentName = safeAttachmentName(meta?.attachment?.fileName || "");
    if (!line && !attachmentName) return;
    const payload = {
          type: "forkmesh:world-chat",
          id: String(meta?.id || "").slice(0, 96),
          sender: String(sender || "").slice(0, meta?.via === true ? 96 : MAX_NAME),
          self: senderId === selfId,
          own: isOwnChatLine(sender, senderId),
          account: meta?.account === true,
          bot: meta?.bot === true && meta?.via !== true,
          via: meta?.via === true,
          integration: meta?.integration === true,
          text: line,
          ts: Number(meta?.ts) || Date.now(),
          attachmentName: attachmentName === "file" ? "" : attachmentName,
          attachmentMime: safeAttachmentMime(
            meta?.attachment?.fileMime || "",
          ).slice(0, MAX_ATTACHMENT_MIME),
          reactionCount: Math.max(
            0,
            Math.min(999, Number(meta?.reactionCount) || 0),
          ),
          history,
    };
    const post = (attachmentPreview = "") => {
      try {
        dispatchWorldChatPayload({ ...payload, attachmentPreview });
      } catch (_) {}
    };
    post();
    if (meta?.attachment && !history) {
      void worldAttachmentPreview(meta.attachment).then((preview) => {
        if (preview) post(preview);
      });
    }
  }

  // Replayed entries can arrive out of order. Keep their newest surviving
  // record for the physical World board; mutations are applied before this is
  // emitted, so an edited/deleted latest line never leaks as stale activity.
  let newestHistoryTs = 0;
  function emitWorldChatHistory(entry) {
    if (!WORLD_EMBED_BUBBLES) return;
    const ts = Number(entry.ts) || 0;
    newestHistoryTs = Math.max(newestHistoryTs, ts);
    emitWorldChatBubble(
      entry.sender,
      entry.senderId,
      entry.text,
      true,
      { ...entry, account: false, bot: false, integration: false },
    );
  }

  function emitNewestWorldChatHistory() {
    const record = currentHistoryRows().reduce((newest, candidate) => {
      if (!newest || Number(candidate.tsMs) >= Number(newest.tsMs)) {
        return candidate;
      }
      return newest;
    }, null);
    if (!record) return;
    const reactionCount = [...(reactions.get(record.id)?.values() || [])]
      .reduce((count, reactors) => count + reactors.size, 0);
    emitWorldChatHistory({
      id: record.id,
      sender: record.who,
      senderId: record.senderId,
      text: record.text,
      ts: record.tsMs,
      attachment: record.attachment,
      reactionCount,
    });
  }

  function allowedChatAccountKind(value) {
    return value === "user" || value === "guest" || value === "integration";
  }

  function normalizedPublicWorldFrame(entry) {
    if (!PUBLIC_WORLD_GENERAL || !entry || typeof entry !== "object") {
      return entry;
    }
    return {
      ...entry,
      accountKind: ["user", "integration"].includes(entry.accountKind) ? entry.accountKind : "guest",
      sender: entry.botVia ? entry.sender : publicWorldName(entry.sender),
    };
  }

  function renderChatEntry(entry, kind, live = false) {
    if (!entry || !allowedChatAccountKind(entry.accountKind) || !isAuthenticatedPlain(entry)) return;
    entry = normalizedPublicWorldFrame(botLine(entry));
    if (
      orgAgentIdentity(entry.sender, entry.senderId) &&
      !orgAgentConversationAccess
    ) {
      return;
    }
    if (!once(entry.id)) return;
    if (chatRetention?.isExpired(entry.ts, CHANNEL)) return;
    const who = entry.botVia ? String(entry.sender) : String(entry.sender || "peer").slice(0, MAX_NAME);
    const text = entry.text || "";
    const attachment = attachmentFromEntry(entry);
    if (!text && !attachment) return;
    kind = entry.senderId === selfId ? "self" : kind;
    // Anyone who has spoken here — including in replayed history — becomes a
    // composer mention candidate.
    if (entry.senderId !== selfId) {
      rememberMentionPerson(who, live ? Date.now() : Number(entry.ts) || 0);
    }
    appendMessage(
      kind,
      who,
      text,
      entry.id,
      entry.senderId,
      Number(entry.ts) || Date.now(),
      attachment,
      !live,
      String(entry.accountKind || ""),
    );
    if (live) {
      newestHistoryTs = Math.max(newestHistoryTs, Number(entry.ts) || 0);
      emitWorldChatBubble(who, entry.senderId, text, false, {
        id: entry.id,
        ts: entry.ts,
        attachment,
        reactionCount: entry.reactionCount,
        account: entry.accountKind === "user" && Boolean(entry.verifiedAccount) && !entry.postedBy,
        via: Boolean(entry.botVia),
        integration: entry.accountKind === "integration",
      });
    }
  }

  async function verifyAdminDelete(plain) {
    const target = plain.target;
    const adminId = plain.senderId;
    const sig = plain.sig;
    if (!target || !sig || !adminId) return false;
    const canonical =
      "forkmesh-admin-delete-v1\n" +
      (plain.conversation || "") +
      "\n" +
      target +
      "\n" +
      adminId +
      "\n" +
      String(plain.ts);
    if (!(await ed25519Verify(adminId, sig, canonical))) return false;
    try {
      const name = String(plain.sender || "").trim().toLowerCase();
      if (!name) return false;
      const res = await fetch("/api/accounts/" + encodeURIComponent(name));
      const rec = await res.json();
      return rec && rec.isAdmin === true && rec.pubkey === adminId;
    } catch (_) {
      return false;
    }
  }

  function finishHistoryReplay() {
    renderHistoryWindow();
    renderSideMessages();
    emitNewestWorldChatHistory();
  }

  function handlePlain(plain, historyReplay = false) {
    if (!isAuthenticatedPlain(plain)) return;
    plain = botLine(plain);
    const type = plain.type;
    // Mirror-mesh signals ride the same encrypted room as chat: a source node
    // broadcasts "mirror-update" the instant its repo advances from the source
    // of truth, and every mirror node replies "mirror-synced" once it has
    // pulled that commit. Re-broadcast both as a window event so the dashboard's
    // open repo can re-fetch host health live instead of waiting for a manual
    // reload — this is what makes the Mirrors tab show nodes converge instantly.
    // These frames are stamped accountKind "node", so handle them before the
    // user-only guard below (which is meant for chat surfaces).
    if (type === "mirror-update" || type === "mirror-synced") {
      try {
        window.dispatchEvent(new CustomEvent("forkmesh:mirror-signal", {
          detail: {
            kind: type,
            repo: String(plain.repo || "").slice(0, 200),
            commit: String(plain.commit || "").slice(0, 64),
            sender: String(plain.sender || "").slice(0, MAX_NAME),
          },
        }));
      } catch (_) {}
      return;
    }
    if (!allowedChatAccountKind(plain.accountKind)) return;
    plain = normalizedPublicWorldFrame(plain);
    const sender = String(plain.sender || "peer").slice(0, MAX_NAME);
    // "hello"/"presence" frames are the only sign of someone who is here but
    // has not typed yet; keep them in the mention list too.
    if (plain.senderId !== selfId) {
      rememberMentionPerson(
        sender,
        historyReplay ? Number(plain.ts) || 0 : Date.now(),
      );
      if (
        !historyReplay &&
        plain.accountKind === "user" &&
        (type === "hello" || type === "presence") &&
        plain.userStatus
      ) {
        window.ForkMeshUserStatus?.remember(sender, plain.userStatus, "frame");
      }
    }
    if (type === "chat") {
      if (plain.channel === CHANNEL) {
        renderChatEntry(plain, "peer", !historyReplay);
      }
    } else if (type === "thread-reply") {
      renderThreadEntry(plain, "peer", !historyReplay);
    } else if (type === "history") {
      const entries = frameAuth.historyEntries(plain);
      for (const entry of entries) {
        if (
          entry &&
          (!entry.type || entry.type === "chat") &&
          entry.channel === CHANNEL &&
          (entry.channel || entry.text || entry.fileName)
        ) {
          renderChatEntry(entry, "peer");
        }
      }
      for (const entry of entries) {
        if (entry?.type && entry.type !== "chat") {
          handlePlain(entry, true);
        }
      }
      if (!historyReplay) finishHistoryReplay();
    } else if (type === "reaction") {
      if (frameAuth.isSealedPlain(plain) && once(plain.id)) applyReaction(plain);
    } else if (type === "edit") {
      const rec = rows.get(plain.target) || threadRepliesById.get(plain.target);
      if (rec && frameAuth.mayChangeMessage(plain, rec)) {
        rec.text = plain.text || "";
        rec.editedAt = plain.editedAt || plain.ts;
        if (rec.rootId) {
          if (activeThreadRootId === rec.rootId) renderThreadDialog();
        } else if (rec.textEl) {
          renderMessageText(rec.textEl, rec.text);
          markEdited(rec, rec.editedAt);
        }
        const sideEntry = sideEntries.find((entry) => entry.id === plain.target);
        if (sideEntry) {
          sideEntry.text = plain.text || "";
          if (!historyReplay) renderSideMessages();
        }
      }
    } else if (type === "delete") {
      const rec = rows.get(plain.target) || threadRepliesById.get(plain.target);
      if (rec && frameAuth.mayChangeMessage(plain, rec)) {
        removeMessage(plain.target, { deferRender: historyReplay });
      }
    } else if (type === "admin-delete") {
      verifyAdminDelete(plain).then((ok) => {
        if (!ok) return;
        seen.add(plain.target);
        removeMessage(plain.target);
      });
    } else if (type === "hello" && !plain.to) {
      // Presence is shown in the World itself; do not add join noise to the
      // message timeline or push the composer upward.
    } else if (type === "bye") {
      appendSystem(sender + " left");
    }
  }

  async function flushHistoryReplay() {
    if (historyReplayTimer) {
      clearTimeout(historyReplayTimer);
      historyReplayTimer = 0;
    }
    const envelopes = historyReplayEnvelopes;
    historyReplayEnvelopes = [];
    if (!envelopes.length) {
      finishHistoryReplay();
      return;
    }
    const frames = await Promise.all(
      envelopes.map((envelope) => decryptObject(envelope, true)),
    );
    const decoded = await attributedPlains(frames);
    // D1 ties frames stored in the same millisecond by opaque cipher hash, so
    // an edit/delete can replay before its original chat. Register every chat
    // record first, then apply mutations in the received order.
    for (const plain of decoded) {
      if (plain.type === "chat") handlePlain(plain, true);
    }
    for (const plain of decoded) {
      if (plain.type !== "chat") handlePlain(plain, true);
    }
    finishHistoryReplay();
  }

  function scheduleHistoryReplayFallback() {
    if (historyReplayTimer) clearTimeout(historyReplayTimer);
    historyReplayTimer = window.setTimeout(() => {
      historyReplayTimer = 0;
      inboundFrameQueue = inboundFrameQueue
        .then(() => flushHistoryReplay())
        .catch(() => {});
    }, 180);
  }

  // The relay is the only sender of "forkmesh-new-member": it signs the frame
  // with a secret no browser holds, and the room drops every non-cipher frame
  // a peer tries to send, so this celebration can only be started by a real
  // signup. The overlay is the shared one every chat surface uses.
  // Loaded on the first announcement rather than at mount: signups are rare,
  // and chat should not pay a request per page for a party that may not come.
  let memberWelcomePromise = null;

  function celebrateNewMember(name) {
    if (!memberWelcomePromise) {
      memberWelcomePromise = import("./member-welcome.js").catch(() => null);
    }
    void memberWelcomePromise.then((module) => {
      if (!module?.celebrateNewMember?.(name)) return;
      appendSystem(
        module.NEW_MEMBER_GREETING(String(name).trim().toLowerCase()),
      );
    });
  }

  async function processFrameData(data) {
    if (typeof data !== "string") return;
    let envelope;
    try {
      envelope = JSON.parse(data);
    } catch (_) {
      return;
    }
    if (envelope?.kind === "forkmesh-history-end") {
      await flushHistoryReplay();
      return;
    }
    if (envelope?.kind === "forkmesh-new-member") {
      celebrateNewMember(envelope.name);
      return;
    }
    if (envelope?.historyReplay === true) {
      historyReplayEnvelopes.push(envelope);
      scheduleHistoryReplayFallback();
      return;
    }
    const plain = await moderateDashboardPlain(await decryptObject(envelope));
    if (!plain) return;
    const [attributed] = await attributedPlains([plain]);
    if (attributed) handlePlain(attributed);
  }

  async function onFrame(event) {
    const data = event.data;
    const queued = inboundFrameQueue.then(() => processFrameData(data));
    inboundFrameQueue = queued.catch(() => {});
    await queued;
  }

  // Durable message types the relay should retain (still encrypted) and replay
  // to clients that join later — the same set the desktop node tags via
  // kDurableTypes (ServerNode::sendEncrypted). Without this flag the relay's
  // _maybe_retain drops the frame, so a message typed on the website is relayed
  // live but never becomes part of the shared history nodes and other web
  // visitors see on connect — leaving the website out of the shared chat.
  const DURABLE_TYPES = new Set([
    "chat", "thread-reply", "edit", "delete", "reaction", "admin-delete",
  ]);
  const NOTIFY_TYPES = new Set(["chat", "thread-reply"]);
  let chatBadgeActivityAccount = "";

  function recordOwnChatBadgeActivity(plain) {
    // The relay intentionally retains only ciphertext. Once an authenticated
    // person has actually handed their own chat line to an open socket, record
    // the one durable, content-free achievement companion request. Automated
    // ForkBot lines and non-message mutations do not qualify.
    if (
      !["chat", "thread-reply"].includes(plain?.type) ||
      plain?.senderId !== selfId
    ) {
      return;
    }
    const session = userSession();
    if (!session) return;
    const account = String(session.nodeName || "").trim().toLowerCase();
    if (!account || chatBadgeActivityAccount === account) return;
    chatBadgeActivityAccount = account;
    const token = String(session.sessionToken || "").trim();
    const headers = { "content-type": "application/json" };
    if (token && token !== "cookie") {
      headers.authorization = `Bearer ${token}`;
    }
    void fetch("/api/chat/activity", {
      method: "POST",
      cache: "no-store",
      credentials: "same-origin",
      headers,
      body: JSON.stringify(
        token && token !== "cookie" ? { sessionToken: token } : {},
      ),
    }).then((response) => {
      if (!response.ok && chatBadgeActivityAccount === account) {
        chatBadgeActivityAccount = "";
      }
    }).catch(() => {
      if (chatBadgeActivityAccount === account) chatBadgeActivityAccount = "";
    });
  }

  function send(plain) {
    if (!canJoinChat()) {
      showUserOnlyState();
      return Promise.resolve();
    }
    return moderateDashboardPlain(plain).then((moderated) => {
      if (!moderated) {
        setStatus("Chat filter unavailable · message not sent");
        return null;
      }
      return encryptObject(moderated).catch(() => null);
    }).then((envelope) => {
      if (!envelope) return;
      if (DURABLE_TYPES.has(plain && plain.type)) envelope.persist = true;
      // Retained edits and reactions stay quiet; only a message wakes the
      // room's phones (see push_chat_room_apns in the Worker).
      if (NOTIFY_TYPES.has(plain && plain.type)) envelope.notify = true;
      if (socket && socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify(envelope));
        recordOwnChatBadgeActivity(plain);
        // You have already read what you just typed, so it must not light the
        // site-header chat badge on every other page. That badge is a delta
        // against a stored baseline, and only retained public-world frames
        // reach the counter behind it (oversized file frames are dropped
        // before retention).
        if (envelope.persist && PUBLIC_WORLD_GENERAL && !plain.file) {
          noteOwnChatActivity();
        }
      }
    });
  }

  // The relay's room DO reaps sockets that send nothing for 3 minutes, and an
  // idle dashboard tab used to send nothing — its chat silently went stale
  // (no new messages) until the user typed. Reconnect with backoff and beat
  // presence at the desktop's 60s cadence to stay live (mirrors chat.js).
  let reconnectDelayMs = 2000;
  let reconnectTimer = null;
  let roomGeneration = 0;

  function scheduleReconnect() {
    if (reconnectTimer || !canJoinChat()) return;
    setStatus("Disconnected · reconnecting…");
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      connect();
    }, reconnectDelayMs);
    reconnectDelayMs = Math.min(reconnectDelayMs * 2, 30000);
  }

  async function connect() {
    if (socket || connecting) return;
    const generation = roomGeneration;
    if (!canJoinChat()) {
      showUserOnlyState();
      return;
    }
    // WebCrypto (crypto.subtle) only exists in a secure context. On plain HTTP
    // — a self-hosted node or LAN IP opened on mobile — it is undefined, so the
    // room key can never derive. Say so plainly instead of the old blanket
    // "Encryption unavailable", which read like a transient glitch.
    if (!window.isSecureContext || !(window.crypto && window.crypto.subtle)) {
      setStatus("Chat needs a secure (HTTPS) connection");
      return;
    }
    connecting = true;
    setInputsEnabled(true);
    setStatus("Connecting...");
    try {
      if (!roomKey) {
        const derived = await deriveRoomKey();
        if (generation !== roomGeneration) {
          connecting = false;
          return;
        }
        roomKey = derived;
      }
      // Re-resolved on every connect so public keys learned by another browser
      // surface are immediately available here.
      await loadRoomSeal();
      if (sealUnavailable) {
        connecting = false;
        setStatus("End-to-end encryption unavailable · reload to join");
        return;
      }
    } catch (err) {
      connecting = false;
      // An expired/absent session token 401s the room-key fetch; tell the user
      // to sign in again rather than blaming encryption.
      setStatus(err && err.code === "auth" ? "Sign in again to join chat" : "Encryption unavailable");
      return;
    }
    if (generation !== roomGeneration) {
      connecting = false;
      return;
    }
    const scheme = location.protocol === "https:" ? "wss:" : "ws:";
    const opened = new WebSocket(`${scheme}//${RELAY_HOST}${CHAT_WS_PATH}`);
    socket = opened;
    socket.addEventListener("open", async () => {
      if (socket !== opened) return;
      connecting = false;
      reconnectDelayMs = 2000;
      setStatus(
        PUBLIC_WORLD_GENERAL
          ? "Connected · public World #general"
          : "Connected · authenticated shared key"
      );
      await sendSealAdvertisement(
        Boolean(roomSeal?.shouldPersistAdvertisement?.()),
      );
      await send(makePlain("hello", { channels: [CHANNEL] }));
      const callbacks = openCallbacks;
      openCallbacks = [];
      callbacks.forEach((cb) => cb());
    });
    socket.addEventListener("message", (event) => {
      if (socket === opened) onFrame(event);
    });
    socket.addEventListener("close", () => {
      if (socket !== opened) return;
      socket = null;
      connecting = false;
      scheduleReconnect();
    });
    socket.addEventListener("error", () => {
      opened.close();
    });
  }

  function switchChatSpace(space) {
    const next = String(space || "");
    if (requestedOrganization) return false;
    if (next && !Object.hasOwn(SPACE_CHANNELS, next)) return false;
    if (next === ACTIVE_SPACE) return true;
    roomGeneration += 1;
    const previous = socket;
    socket = null;
    connecting = false;
    openCallbacks = [];
    if (reconnectTimer) clearTimeout(reconnectTimer);
    reconnectTimer = null;
    reconnectDelayMs = 2000;
    if (previous) {
      try {
        previous.close();
      } catch (_) {}
    }
    if (historyReplayTimer) clearTimeout(historyReplayTimer);
    historyReplayTimer = 0;
    historyReplayEnvelopes = [];
    inboundFrameQueue = Promise.resolve();
    configureChatSpace(next);
    roomKey = null;
    roomPassphrase = null;
    roomSeal = null;
    sealUnavailable = false;
    unreadableSealKeyIds.clear();
    seen.clear();
    rows.clear();
    reactions.clear();
    stickers.clear();
    threadRepliesById.clear();
    threadReplyIdsByRoot.clear();
    activeThreadRootId = "";
    historyRowIds.length = 0;
    liveRowIds.length = 0;
    historyVisibleCount = HISTORY_INITIAL_MESSAGES;
    historyIndicator = null;
    stickToNewest = true;
    recentContext.length = 0;
    sideEntries.length = 0;
    newestHistoryTs = 0;
    if (fullLog) fullLog.replaceChildren();
    if (sideLog) sideLog.replaceChildren();
    if (fullChannel) fullChannel.value = ACTIVE_SPACE;
    syncFullComposerAction();
    ensureEmptyState();
    if (canJoinChat()) {
      setStatus("Not connected");
      connect();
    }
    syncDiscordChrome();
    return true;
  }

  dashboardChatController = {
    switchSpace: switchChatSpace,
    activeSpace: () => ACTIVE_SPACE,
    channel: () => CHANNEL,
    spaces: () => ["", ...Object.keys(SPACE_CHANNELS)].map((space) => ({
      space,
      channel: space ? SPACE_CHANNELS[space] : "#general",
    })),
  };

  setInterval(() => {
    if (socket && socket.readyState === WebSocket.OPEN && canJoinChat()) {
      send(makePlain("presence"));
    }
  }, 60000);

  let chatRetention = null;

  function pruneExpiredChat() {
    if (!chatRetention) return;
    const cutoff = chatRetention.cutoff(CHANNEL);
    const expired = new Set();
    for (const id of historyRowIds) {
      if (Number(rows.get(id)?.tsMs) <= cutoff) expired.add(id);
    }
    for (const entry of sideEntries) {
      if (Number(entry.tsMs) <= cutoff) expired.add(entry.id);
    }
    if (!expired.size) return;
    for (const id of expired) removeMessage(id, { deferRender: true });
    renderSideMessages();
    renderHistoryWindow({ preserveScroll: true });
  }

  chatRetentionPromise.then((module) => {
    if (!module) return;
    let storage = null;
    try {
      storage = window.localStorage;
    } catch (_) {}
    chatRetention = module.createChatRetention({
      storage,
      request: async (body) => {
        const token = String(userSession()?.sessionToken || "").trim();
        const headers = new Headers({
          accept: "application/json",
          "content-type": "application/json",
        });
        if (token && token !== "cookie") headers.set("authorization", `Bearer ${token}`);
        const response = await fetch(module.CHAT_RETENTION_ENDPOINT, {
          method: "POST",
          headers,
          body: JSON.stringify(body),
          credentials: "same-origin",
          cache: "no-store",
        });
        if (!response.ok) throw new Error("chat_retention_unavailable");
        return response.json();
      },
    });
    chatRetention.apply(userSession()?.chatRetention);
    chatRetention.onChange(pruneExpiredChat);
    pruneExpiredChat();
    setInterval(pruneExpiredChat, 60000);
    if (userSession()?.sessionToken) chatRetention.refresh().catch(() => {});
  });

  // Status changes: announce our own at once, and repaint the author badges
  // already on screen (rows are built once). The full chat page also loads
  // the directory up front so offline members' statuses show too.
  if (window.ForkMeshUserStatus) {
    window.addEventListener("forkmesh:user-status-self", () => {
      if (socket && socket.readyState === WebSocket.OPEN && canJoinChat()) {
        send(makePlain("presence"));
      }
    });
    window.ForkMeshUserStatus.subscribe(() => {
      for (const author of document.querySelectorAll("[data-user-status-name]")) {
        if (author.nextElementSibling?.classList.contains("fm-user-status-badge")) {
          author.nextElementSibling.remove();
        }
        const badge = window.ForkMeshUserStatus.badge(
          window.ForkMeshUserStatus.forName(author.dataset.userStatusName));
        if (badge) author.after(badge);
      }
    });
    if (fullLog) void refreshMentionDirectory();
  }

  function runWhenConnected(callback) {
    if (socket && socket.readyState === WebSocket.OPEN) {
      callback();
      return;
    }
    openCallbacks.push(callback);
    connect();
  }

  function makeBotPlain(text, sender = FORKBOT_SENDER_ID) {
    const botName = [FORKBOT_SENDER_ID, CLAUDE_SENDER_ID, CODEX_SENDER_ID]
      .includes(sender) ? sender : FORKBOT_SENDER_ID;
    return makePlain("chat", {
      channel: CHANNEL,
      text: String(text || "").slice(0, MAX_TEXT),
      sender: botName,
      senderId: selfId,
      bot: botName,
      accountKind: "user",
    });
  }

  function botLine(entry) {
    if (!frameAuth) return entry;
    return frameAuth.botLine(entry, [FORKBOT_SENDER_ID, CLAUDE_SENDER_ID, CODEX_SENDER_ID, ORG_BOT_SENDER_ID]);
  }

  async function broadcastBotMessage(text, sender = FORKBOT_SENDER_ID) {
    const moderation = await moderateDashboardText(text);
    if (!moderation) return;
    const plain = makeBotPlain(moderation.text, sender);
    // Claude/Codex prompts and replies are intentionally absent from the
    // shared room. The Engineering-only session endpoint remains the durable
    // transcript; this local line is merely immediate feedback to its author.
    if (!orgAgentIdentity(plain.sender, plain.bot)) send(plain);
    seen.add(plain.id);
    appendMessage("peer", plain.bot, plain.text, plain.id, plain.bot, plain.ts);
    // The asking client appends directly (not via renderChatEntry), so mirror
    // the reply to the World embed here too.
    emitWorldChatBubble(plain.bot, plain.bot, plain.text, false, { bot: true });
  }

  async function maybeAskForkbot(text) {
    // Anyone who can join the room can talk to ForkBot: signed-in users on
    // the dashboard, and guests inside the public World room (the endpoint
    // itself is sessionless).
    if (!canJoinChat()) return;
    if (!FORKBOT_MENTION_RE.test(text || "")) return;
    if (!PUBLIC_WORLD_GENERAL) {
      appendSystem(
        "forkbot is available only in public, controller-readable channels",
      );
      return;
    }
    // Drop the triggering line (sent separately as `message`) and ForkBot's own
    // replies, and cap the rest so ForkBot sees the lead-up conversation.
    const context = recentContext
      .slice(0, -1)
      .filter((m) => m.sender.toLowerCase() !== "forkbot" && !frameAuth?.isBotViaName(m.sender))
      .slice(-12);
    try {
      const response = await fetch(FORKBOT_ENDPOINT, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({
          message: text,
          sender: displayName(),
          room: ACTIVE_SPACE || ROOM_NAME,
          context,
          model: forkbotModel(),
        }),
      });
      const data = await response.json().catch(() => ({}));
      if (!response.ok || !data || !data.botMessage) return;
      runWhenConnected(() => broadcastBotMessage(data.botMessage));
    } catch (_) {
      appendSystem("forkbot is unavailable");
    }
  }

  function orgAgentScope(override = null) {
    const selectedOwner = String(override?.owner || "");
    const selectedRepo = String(override?.repo || override?.name || "");
    if (
      /^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(selectedOwner) &&
      /^[A-Za-z0-9._-]{1,100}$/.test(selectedRepo)
    ) {
      return {
        organization: selectedOwner,
        owner: selectedOwner,
        repo: selectedRepo,
      };
    }
    const organization = requestedOrganization || "forkmesh";
    return { organization, owner: organization, repo: "forkmesh" };
  }

  const ORG_AGENT_ERROR_MESSAGES = Object.freeze({
    invalid_session: "Your session is no longer valid. Sign in again.",
    forbidden: "You do not have permission to queue this agent.",
    engineering_team_required:
      "Engineering team membership is required to view these agent sessions.",
    org_owner_required:
      "Organization owner access is required to queue work for this desktop.",
    repository_not_linked:
      "This repository is not linked to the organization.",
    repository_not_published:
      "The linked desktop has not published this repository yet.",
    target_not_owned:
      "The selected desktop is not owned by this organization owner.",
    target_not_desktop:
      "The selected target is not a desktop runtime.",
    provider_not_advertised:
      "The linked desktop has not advertised this agent provider.",
    preferred_target_ineligible:
      "The selected desktop is not eligible for this agent request.",
    no_online_agent_mirror:
      "No eligible online agent mirror is available.",
    no_eligible_agent_node:
      "No eligible agent node is available for this repository.",
    invalid_provider: "Choose Claude Code or Codex.",
    invalid_model: "That model is not available for the selected provider.",
    prompt_required: "Add a task for the agent.",
    invalid_target_node: "The selected target node is not valid.",
    invalid_task_key: "The task key is not valid.",
    invalid_issue_number: "The issue number is not valid.",
    invalid_issue_task_key: "The issue task key is not valid.",
    session_not_promptable:
      "This agent session is not accepting another prompt.",
    agent_not_ready: "The agent is not ready for another prompt.",
    queue_persistence_failed:
      "The agent request could not be saved. Try again.",
  });

  function jsonResponseError(response, payload, fallback = "Request failed.") {
    const details = payload && typeof payload === "object" ? payload : {};
    const code = String(details.error || "").trim();
    const serverMessage = String(details.message || "").trim();
    const message =
      serverMessage ||
      ORG_AGENT_ERROR_MESSAGES[code] ||
      code ||
      (response?.status ? `Request returned ${response.status}.` : fallback);
    const error = new Error(message);
    error.code = code;
    error.status = Number(response?.status || 0);
    error.payload = details;
    for (const key of [
      "requiredTeam",
      "targetNode",
      "provider",
      "requiredAction",
      "retryable",
    ]) {
      if (details[key] !== undefined) error[key] = details[key];
    }
    return error;
  }

  function orgAgentSavedMessage(payload) {
    const serverMessage = String(payload?.message || "").trim();
    if (serverMessage) return serverMessage;
    const target = String(
      payload?.targetNode ||
        payload?.session?.targetNode ||
        "the linked desktop",
    );
    const waitingForDesktop =
      payload?.targetOnline === false ||
      payload?.queueState === "waiting_for_desktop";
    return waitingForDesktop
      ? `Saved for ${target}. It will start after the linked desktop reconnects.`
      : `Saved for ${target}. It will start when the node claims it.`;
  }

  function savedTaskQueueError(error) {
    const failure = new Error(
      `The task was saved, but the agent was not queued: ${String(
        error?.message || "The queue request failed.",
      )}`,
    );
    failure.code = String(error?.code || "");
    failure.status = Number(error?.status || 0);
    failure.payload = error?.payload;
    return failure;
  }

  async function loadOrgAgentChatAccess() {
    orgAgentEngineeringAccess = false;
    orgAgentQueueAccess = false;
    orgAgentConversationAccess = false;
    orgAgentAccessReason = "";
    orgAgentAccessLoaded = false;
    const session = userSession();
    if (!session) {
      orgAgentAccessLoaded = true;
      return false;
    }
    const scope = orgAgentScope();
    const endpoint =
      `/api/orgs/${encodeURIComponent(scope.organization)}` +
      `/repos/${encodeURIComponent(scope.repo)}/agent-bots`;
    const token = String(session.sessionToken || "");
    const headers = { accept: "application/json" };
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    try {
      const response = await fetch(endpoint, {
        cache: "no-store",
        credentials: "same-origin",
        headers,
      });
      const data = await response.json().catch(() => ({}));
      if (!response.ok || data?.ok === false) {
        throw jsonResponseError(
          response,
          data,
          "Agent access could not be checked.",
        );
      }
      orgAgentEngineeringAccess = data?.engineeringAccess === true;
      orgAgentQueueAccess =
        data?.canQueueAgent === true || orgAgentEngineeringAccess;
      orgAgentConversationAccess =
        data?.canViewAgentSessions === true ||
        orgAgentEngineeringAccess ||
        orgAgentQueueAccess;
      orgAgentAccessReason = String(
        data?.accessReason || data?.message || "",
      ).trim();
    } catch (error) {
      orgAgentEngineeringAccess = false;
      orgAgentQueueAccess = false;
      orgAgentConversationAccess = false;
      orgAgentAccessReason = String(
        error?.message || "Agent access could not be checked.",
      );
    }
    orgAgentAccessLoaded = true;
    return orgAgentQueueAccess;
  }

  async function maybeAskOrgAgent(text, selectedScope = null) {
    const provider = CLAUDE_MENTION_RE.test(text || "")
      ? "claude-code"
      : CODEX_MENTION_RE.test(text || "")
        ? "codex"
        : "";
    if (!provider) return false;
    const botName = provider === "codex" ? CODEX_SENDER_ID : CLAUDE_SENDER_ID;
    if (!userSession()) {
      appendSystem("Sign in before asking an organization agent.");
      return false;
    }
    const prompt = String(text || "")
      .replace(provider === "codex" ? CODEX_MENTION_RE : CLAUDE_MENTION_RE, " ")
      .trim();
    if (!prompt) {
      appendSystem(`Add a task after @${botName}.`);
      return false;
    }
    try {
      return await queueOrgAgent(provider, prompt, botName, selectedScope);
    } catch (error) {
      appendSystem(
        String(
          error?.message ||
            orgAgentAccessReason ||
            "The agent request failed.",
        ),
      );
      return false;
    }
  }

  // One queue path for every bot request: the mention shortcuts pass a named
  // provider, the task board passes "agent" and lets the Worker pick whichever
  // runtime has an eligible mirror online.
  async function queueOrgAgent(
    provider,
    prompt,
    botName = ORG_BOT_SENDER_ID,
    selectedScope = null,
  ) {
    const session = userSession();
    if (!session) {
      throw new Error("Sign in before starting an organization agent.");
    }
    if (!prompt) throw new Error("Add a task for the agent.");
    const scope = orgAgentScope(selectedScope);
    const taskKeyMatch = prompt.match(
      /\[(task:[a-z0-9-]{1,48}|issue:[a-z0-9-]{1,40}\/[a-z0-9._-]{1,60}#[1-9][0-9]{0,8})\]/i,
    );
    const endpoint =
      `/api/orgs/${encodeURIComponent(scope.organization)}` +
      `/repos/${encodeURIComponent(scope.repo)}/agent-bots`;
    const token = String(session.sessionToken || "");
    const headers = {
      "content-type": "application/json",
      accept: "application/json",
    };
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    const response = await fetch(endpoint, {
      method: "POST",
      cache: "no-store",
      credentials: "same-origin",
      headers,
      body: JSON.stringify({
        provider,
        prompt: prompt.slice(0, 8000),
        ...(taskKeyMatch ? { taskKey: taskKeyMatch[1].toLowerCase() } : {}),
        ...(token && token !== "cookie" ? { sessionToken: token } : {}),
      }),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data?.ok === false) {
      throw jsonResponseError(response, data, "The agent request failed.");
    }
    orgAgentQueueAccess = true;
    orgAgentConversationAccess = true;
    const target = String(
      data?.targetNode || data?.session?.targetNode || "the linked desktop",
    );
    broadcastBotMessage(orgAgentSavedMessage({ ...data, targetNode: target }), botName);
    return data;
  }

  function showAttachmentFeedback(control, message) {
    if (!control?.feedback) return;
    control.feedback.textContent = String(message || "");
    if (message) {
      setTimeout(() => {
        if (control.feedback.textContent === message) {
          control.feedback.textContent = "";
        }
      }, 5000);
    }
  }

  function renderDashboardDraft(control) {
    if (!control?.queue) return;
    control.queue.textContent = "";
    for (const record of control.draft) {
      const name = safeAttachmentName(record.file.name);
      const row = document.createElement("div");
      row.className = "flex min-w-0 max-w-full items-center gap-2 rounded-md border border-border bg-secondary px-2 py-1.5";
      if (record.previewUrl) {
        const preview = document.createElement("img");
        preview.src = record.previewUrl;
        preview.alt = name;
        preview.className = "h-8 w-8 shrink-0 rounded object-cover";
        row.append(preview);
      }
      const copy = document.createElement("div");
      copy.className = "min-w-0 flex-1";
      const title = document.createElement("div");
      title.className = "truncate text-xs font-semibold text-foreground";
      title.textContent = name;
      const meta = document.createElement("div");
      meta.className = "truncate text-[10px] text-muted-foreground";
      meta.textContent = `${safeAttachmentMime(record.file.type)} - ${formatAttachmentSize(record.file.size)}`;
      copy.append(title, meta);
      const remove = document.createElement("button");
      remove.type = "button";
      remove.className = "shrink-0 rounded p-1 text-muted-foreground hover:bg-background hover:text-foreground";
      remove.textContent = "×";
      remove.setAttribute("aria-label", `Remove ${name}`);
      remove.addEventListener("click", () => {
        const index = control.draft.indexOf(record);
        if (index < 0) return;
        control.draft.splice(index, 1);
        if (record.previewUrl) URL.revokeObjectURL(record.previewUrl);
        renderDashboardDraft(control);
      });
      row.append(copy, remove);
      control.queue.append(row);
    }
  }

  function canvasBlob(canvas, type, quality) {
    return new Promise((resolve) => canvas.toBlob(resolve, type, quality));
  }

  async function fitDashboardImage(file) {
    if (
      file.size <= MAX_ATTACHMENT_BYTES ||
      !String(file.type || "").startsWith("image/")
    ) {
      return file;
    }
    if (file.size > 24 * 1024 * 1024) return null;
    let bitmap;
    let objectUrl = "";
    try {
      if (typeof createImageBitmap === "function") {
        bitmap = await createImageBitmap(file);
      } else {
        objectUrl = URL.createObjectURL(file);
        bitmap = await new Promise((resolve, reject) => {
          const image = new Image();
          image.onload = () => resolve(image);
          image.onerror = () => reject(new Error("image_decode_failed"));
          image.src = objectUrl;
        });
      }
      const sourceWidth = Math.max(
        1,
        Number(bitmap.width || bitmap.naturalWidth) || 1,
      );
      const sourceHeight = Math.max(
        1,
        Number(bitmap.height || bitmap.naturalHeight) || 1,
      );
      for (const edge of [1600, 1280, 1024, 800, 640, 480]) {
        const scale = Math.min(1, edge / Math.max(sourceWidth, sourceHeight));
        const canvas = document.createElement("canvas");
        canvas.width = Math.max(1, Math.round(sourceWidth * scale));
        canvas.height = Math.max(1, Math.round(sourceHeight * scale));
        const context = canvas.getContext("2d", { alpha: false });
        if (!context) return null;
        context.fillStyle = "#ffffff";
        context.fillRect(0, 0, canvas.width, canvas.height);
        context.drawImage(bitmap, 0, 0, canvas.width, canvas.height);
        for (const quality of [0.86, 0.76, 0.66, 0.54]) {
          const blob = await canvasBlob(canvas, "image/webp", quality);
          if (blob && blob.size <= MAX_ATTACHMENT_BYTES) {
            const stem = safeAttachmentName(file.name)
              .replace(/\.[^.]+$/, "")
              .slice(0, 170) || "image";
            return new File([blob], `${stem}.webp`, {
              type: "image/webp",
              lastModified: Date.now(),
            });
          }
        }
      }
    } catch (_) {
      return null;
    } finally {
      bitmap?.close?.();
      if (objectUrl) URL.revokeObjectURL(objectUrl);
    }
    return null;
  }

  async function stageDashboardAttachments(control, files) {
    if (!control) return;
    const candidates = Array.from(files || []).filter((file) => file instanceof File);
    const remaining = Math.max(0, 4 - control.draft.length);
    if (candidates.length > remaining) showAttachmentFeedback(control, "Share up to 4 files in a message.");
    for (const source of candidates.slice(0, remaining)) {
      const file = await fitDashboardImage(source);
      if (!file || file.size > MAX_ATTACHMENT_BYTES) {
        showAttachmentFeedback(
          control,
          String(source.type || "").startsWith("image/")
            ? "That image could not be resized to fit the 1 MiB limit."
            : "Attachments must be 1 MiB or smaller.",
        );
        continue;
      }
      if (!file.size) {
        showAttachmentFeedback(control, "That file is empty.");
        continue;
      }
      control.draft.push({
        file,
        previewUrl: String(file.type || "").startsWith("image/")
          ? URL.createObjectURL(file)
          : "",
      });
    }
    renderDashboardDraft(control);
  }

  async function sendAttachment(file, control = null) {
    if (!file || !canJoinChat()) {
      if (!canJoinChat()) showUserOnlyState();
      return false;
    }
    if (file.size > MAX_ATTACHMENT_BYTES) {
      showAttachmentFeedback(control, "Attachments must be 1 MiB or smaller.");
      return false;
    }
    if (!file.size) {
      showAttachmentFeedback(control, "That file is empty.");
      return false;
    }
    let buffer;
    try {
      buffer = await file.arrayBuffer();
    } catch (_) {
      showAttachmentFeedback(control, "Could not read that attachment.");
      return false;
    }
    const fileName = safeAttachmentName(file.name);
    const fileMime = safeAttachmentMime(file.type);
    const encodedFile = bytesToB64(buffer);
    runWhenConnected(() => {
      const plain = makePlain("chat", {
        channel: CHANNEL,
        fileName,
        fileMime,
        file: encodedFile,
      });
      const attachment = attachmentFromEntry(plain);
      if (!attachment) {
        showAttachmentFeedback(control, "Could not prepare that attachment.");
        return;
      }
      send(plain);
      seen.add(plain.id);
      appendMessage(
        "self",
        plain.sender,
        "",
        plain.id,
        plain.senderId,
        plain.ts,
        attachment,
      );
      showAttachmentFeedback(control, `Shared ${fileName}`);
    });
    return true;
  }

  async function sendDashboardDraft(control) {
    const records = [...(control?.draft || [])];
    if (!records.length) return false;
    let sent = false;
    for (const record of records) sent = (await sendAttachment(record.file, control)) || sent;
    if (sent) {
      for (const record of control.draft) if (record.previewUrl) URL.revokeObjectURL(record.previewUrl);
      control.draft = [];
      renderDashboardDraft(control);
    }
    return sent;
  }

  function clearDashboardDraft(control) {
    if (!control) return;
    for (const record of control.draft || []) {
      if (record.previewUrl) URL.revokeObjectURL(record.previewUrl);
    }
    control.draft = [];
    renderDashboardDraft(control);
  }

  async function taskImageAttachments(control) {
    const records = [...(control?.draft || [])];
    const allowed = new Set([
      "image/gif",
      "image/jpeg",
      "image/png",
      "image/webp",
    ]);
    const attachments = [];
    let total = 0;
    for (const { file } of records) {
      const mime = safeAttachmentMime(file?.type).toLowerCase();
      if (!allowed.has(mime)) {
        throw new Error("Tasks accept PNG, JPEG, GIF, or WebP images.");
      }
      const size = Math.max(0, Number(file?.size) || 0);
      if (!size || size > 256 * 1024) {
        throw new Error("Each task image must be 256 KiB or smaller.");
      }
      total += size;
      if (total > 1024 * 1024) {
        throw new Error("Task images must total 1 MiB or less.");
      }
      attachments.push({
        name: safeAttachmentName(file?.name),
        mime,
        size,
        file: bytesToB64(await file.arrayBuffer()),
      });
    }
    return attachments;
  }

  function mountAttachmentControl(inputEl) {
    if (!inputEl?.parentElement) return null;
    const bar = inputEl.parentElement;
    const fileInput = document.createElement("input");
    fileInput.type = "file";
    fileInput.multiple = true;
    fileInput.hidden = true;
    fileInput.id = `${inputEl.id}AttachmentInput`;
    fileInput.setAttribute("aria-label", "Choose image, video, or document");
    const button = document.createElement("button");
    button.type = "button";
    button.className = "shrink-0 inline-flex h-7 w-7 items-center justify-center rounded-md border border-border bg-background text-muted-foreground hover:text-foreground disabled:cursor-not-allowed disabled:opacity-50";
    button.setAttribute("aria-label", "Attach image, video, or document");
    button.title = "Attach image, video, or document (up to 1 MiB)";
    button.dataset.dashboardChatAttach = "";
    button.innerHTML = '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M21.44 11.05 12.25 20.24a6 6 0 0 1-8.49-8.49l9.19-9.19a4 4 0 0 1 5.66 5.66l-9.2 9.19a2 2 0 0 1-2.83-2.83l8.49-8.48"></path></svg>';
    const feedback = document.createElement("span");
    feedback.className = "sr-only";
    feedback.setAttribute("role", "status");
    feedback.setAttribute("aria-live", "polite");
    const queue = document.createElement("div");
    queue.className = "mt-2 flex flex-wrap gap-2";
    queue.dataset.dashboardChatAttachments = "";
    queue.setAttribute("aria-live", "polite");
    queue.setAttribute("aria-label", "Staged attachments");
    const control = {
      button,
      input: fileInput,
      inputEl,
      feedback,
      queue,
      draft: [],
    };
    const sendButton = inputEl === fullInput ? fullSend : sideSend;
    const quickSlot =
      simpleWorldComposer && inputEl === fullInput
        ? bar.querySelector("[data-world-quick-attachment]")
        : null;
    if (quickSlot) {
      // world.css pins the queue to the compose row's second grid row, so it
      // must stay a direct child of the row here.
      bar.parentElement?.insertBefore(queue, bar);
      quickSlot.append(fileInput, button, feedback);
    } else {
      // The resizable composer wraps the textarea in its own div, so the send
      // button is not necessarily a sibling of inputEl: mount beside wherever
      // the send button actually lives and keep the queue above that row.
      const composeRow = bar.closest("[data-dashboard-chat-compose-row]") || bar;
      composeRow.parentElement?.insertBefore(queue, composeRow);
      const host = sendButton?.parentElement || bar;
      host.insertBefore(fileInput, sendButton || null);
      host.insertBefore(button, sendButton || null);
      host.append(feedback);
    }
    button.addEventListener("click", () => fileInput.click());
    fileInput.addEventListener("change", () => {
      const files = Array.from(fileInput.files || []);
      fileInput.value = "";
      if (files.length) {
        void stageDashboardAttachments(control, files).then(() => {
          if (inputEl !== fullInput || String(fullAction?.value || "chat") === "chat") {
            return sendDashboardDraft(control);
          }
          return null;
        });
      }
    });
    control.button.disabled = !canJoinChat();
    control.input.disabled = !canJoinChat();
    attachmentControls.push(control);
    applyWorldComposerPrefill();
    return control;
  }

  // Resolve an "<owner>/<name>" repository picker to the route the API wants.
  // The composer's own picker is the common case; the per-message "issue" form
  // clones it, so the parsing lives here rather than reading fullRepository.
  function repositoryFromSelect(select) {
    const value = String(select?.value || "");
    const match = value.match(
      /^([a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)\/([A-Za-z0-9._-]{1,100})$/,
    );
    if (!match) return null;
    const selected = select?.selectedOptions?.[0];
    const routeOwner = String(selected?.dataset?.routeOwner || match[1]);
    const routeName = String(selected?.dataset?.routeName || match[2]);
    if (
      !/^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(routeOwner) ||
      !/^[A-Za-z0-9._-]{1,100}$/.test(routeName)
    ) {
      return null;
    }
    return {
      owner: routeOwner,
      name: routeName,
      repo: routeName,
      logicalOwner: match[1],
      logicalName: match[2],
      logicalKind: String(selected?.dataset?.logicalKind || "user"),
    };
  }

  function selectedComposerRepository() {
    return repositoryFromSelect(fullRepository);
  }

  function setComposerStatus(message = "", tone = "muted") {
    if (!fullComposerStatus) return;
    fullComposerStatus.textContent = message;
    fullComposerStatus.classList.remove(
      "text-destructive",
      "text-primary",
      "text-muted-foreground",
    );
    fullComposerStatus.classList.add(
      tone === "bad"
        ? "text-destructive"
        : tone === "good"
          ? "text-primary"
          : "text-muted-foreground",
    );
  }

  function pulseWorldQuickComposer(action = "chat") {
    const composer = fullInput?.closest("[data-dashboard-chat-composer]");
    if (!composer) return;
    composer.removeAttribute("data-just-sent");
    composer.setAttribute("data-submitting", action);
    window.setTimeout(() => {
      composer.removeAttribute("data-submitting");
      composer.setAttribute("data-just-sent", action);
      window.setTimeout(
        () => composer.removeAttribute("data-just-sent"),
        720,
      );
    }, 180);
  }

  function setFullComposerBusy(busy) {
    const composer = fullInput?.closest("[data-dashboard-chat-composer]");
    if (composer) {
      composer.toggleAttribute("data-submitting", Boolean(busy));
      if (!busy) {
        composer.setAttribute("data-just-sent", fullAction?.value || "chat");
        window.setTimeout(
          () => composer.removeAttribute("data-just-sent"),
          720,
        );
      }
    }
    for (const control of [
      fullInput,
      fullSend,
      fullTaskSend,
      fullChannel,
      fullRepository,
      fullAction,
      taskDepartment,
      taskTeam,
      taskDestination,
      taskAssignee,
    ]) {
      if (control) control.disabled = Boolean(busy);
    }
  }

  function syncFullComposerAction() {
    if (!fullAction || !fullInput) return;
    const action = fullAction.value;
    if (simpleWorldComposer) {
      if (fullSendLabel) fullSendLabel.textContent = "Chat";
      if (fullComposerHint) {
        fullComposerHint.textContent =
          "Chat posts to #general · Task adds unassigned work to General";
      }
      fullInput.placeholder = `Message ${CHANNEL}…`;
      if (taskRouting) taskRouting.hidden = true;
      fullSend?.setAttribute(
        "aria-pressed",
        String(fullAction.value === "chat"),
      );
      fullTaskSend?.setAttribute(
        "aria-pressed",
        String(fullAction.value !== "chat"),
      );
      syncChatContextBubbles();
      return;
    }
    const presentation = {
      chat: {
        label: "Send",
        hint: "Enter sends · Shift+Enter adds a line",
        placeholder: `Message ${CHANNEL}…`,
      },
      issue: {
        label: "Create issue",
        hint: "First line becomes the title · remaining lines become the description",
        placeholder: "Issue title\\nDescribe the expected behavior and context…",
      },
      task: {
        label: "Create task",
        hint: "First line is the title · route by department, team, destination, and assignee",
        placeholder: "Task title\\nAdd details or QA instructions…",
      },
      agent: {
        label: "Send to bot",
        hint: "Starts a secured Engineering task on an eligible mirror",
        placeholder: "Describe what you want the bot to do…",
      },
    }[action] || {};
    if (fullSendLabel) {
      fullSendLabel.textContent = presentation.label || "Send";
    }
    if (fullComposerHint) fullComposerHint.textContent = presentation.hint || "";
    fullInput.placeholder = presentation.placeholder || "Write a message…";
    if (taskRouting) taskRouting.hidden = action !== "task";
    const repositoryRelevant =
      action !== "chat" &&
      (
        action !== "task" ||
        String(taskDestination?.value || "") === "repository" ||
        AGENT_ASSIGNEE_VALUES.includes(String(taskAssignee?.value || ""))
      );
    fullRepository?.classList.toggle("ring-1", repositoryRelevant);
    fullRepository?.classList.toggle("ring-primary/50", repositoryRelevant);
    syncChatContextBubbles();
    setComposerStatus("");
  }

  function selectedOptionLabel(select, fallback) {
    return String(
      select?.selectedOptions?.[0]?.textContent || fallback,
    ).trim();
  }

  function syncChatContextBubbles() {
    if (contextChannel) {
      contextChannel.textContent = selectedOptionLabel(
        fullChannel,
        CHANNEL_LABEL || "# general",
      );
    }
    if (contextSource) {
      contextSource.textContent = selectedOptionLabel(
        fullRepository,
        "No repository",
      );
    }
    if (contextAction) {
      contextAction.textContent = selectedOptionLabel(fullAction, "Send to chat");
    }
  }

  function syncChatScrollThumb() {
    if (!fullLog || !chatScrollRail) return;
    const available = Math.max(0, fullLog.scrollHeight - fullLog.clientHeight);
    const progress = available > 0
      ? Math.max(0, Math.min(1, fullLog.scrollTop / available))
      : 1;
    chatScrollRail.style.setProperty("--chat-scroll-progress", progress);
    chatScrollRail.toggleAttribute("data-at-start", fullLog.scrollTop <= 1);
    chatScrollRail.toggleAttribute(
      "data-at-end",
      available <= 1 || fullLog.scrollTop >= available - 1,
    );
  }

  async function taskApiRequest(method, path, body = null) {
    const token = String(userSession()?.sessionToken || "");
    const headers = { accept: "application/json" };
    if (body !== null) headers["content-type"] = "application/json";
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    const response = await fetch(path, {
      method,
      cache: "no-store",
      credentials: "same-origin",
      headers,
      ...(body === null
        ? {}
        : {
            body: JSON.stringify({
              ...body,
              ...(token && token !== "cookie"
                ? { sessionToken: token }
                : {}),
            }),
          }),
    });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload?.ok === false) {
      throw jsonResponseError(response, payload, "The task request failed.");
    }
    return payload;
  }

  async function loadTaskRouting() {
    if (!taskAssignee || !taskTeam) return;
    // Task routing is account-scoped: a guest has no session token, so the
    // request can only ever come back 401. Skip it rather than logging an
    // unauthorized fetch (and flashing a status) on every public World load.
    if (!userSession()) return;
    try {
      const payload = await taskApiRequest("GET", "/api/tasks");
      const selfName = String(payload?.actor || displayName()).toLowerCase();
      for (const value of Array.isArray(payload?.members)
        ? payload.members
        : []) {
        const name = String(value || "").trim().toLowerCase();
        if (!/^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(name)) continue;
        const option = document.createElement("option");
        option.value = `user:${name}`;
        option.textContent = name === selfName ? `${name} (you)` : name;
        taskAssignee.append(option);
      }
      if (
        !simpleWorldComposer &&
        selfName &&
        Array.from(taskAssignee.options).some(
          (option) => option.value === `user:${selfName}`,
        )
      ) {
        taskAssignee.value = `user:${selfName}`;
      }
      for (const value of Array.isArray(payload?.teams)
        ? payload.teams
        : []) {
        const team = String(value || "").trim().toLowerCase();
        if (!/^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$/.test(team)) continue;
        const option = document.createElement("option");
        option.value = team;
        option.textContent = team;
        taskTeam.append(option);
      }
    } catch (_) {
      setComposerStatus(
        "Task routing is available only to organization members.",
        "bad",
      );
    }
  }

  async function loadComposerRepositories() {
    if (!fullRepository) return;
    try {
      const response = await fetch("/api/repositories", {
        cache: "no-store",
        credentials: "same-origin",
        headers: { accept: "application/json" },
      });
      const payload = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error("repository_catalog_unavailable");
      const seenRepositories = new Set();
      const repositories = (Array.isArray(payload?.repositories)
        ? payload.repositories
        : [])
        .flatMap((repository) => {
          const routeOwner = String(repository?.owner || "");
          const name = String(repository?.name || repository?.repo || "");
          const logicalOwners = Array.isArray(repository?.logicalOwners)
            ? repository.logicalOwners
            : (
                ["user", "organization"].includes(
                  String(repository?.ownerKind || ""),
                ) && repository?.logicalOwner
                  ? [{
                      kind: repository.ownerKind,
                      owner: repository.logicalOwner,
                    }]
                  : []
              );
          return logicalOwners.map((identity) => {
            const kind = String(identity?.kind || "").toLowerCase();
            const logicalOwner = String(identity?.owner || "").toLowerCase();
            const value = `${logicalOwner}/${name}`;
            const key = `${kind}:${value}`;
            if (
              !["user", "organization"].includes(kind) ||
              !/^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?\/[A-Za-z0-9._-]{1,100}$/.test(
                value,
              ) ||
              !/^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(routeOwner) ||
              seenRepositories.has(key)
            ) {
              return null;
            }
            seenRepositories.add(key);
            return {
              value,
              label: `${value} · ${
                kind === "organization" ? "Organization" : "User"
              }`,
              kind,
              routeOwner:
                kind === "organization" ? logicalOwner : routeOwner,
              routeName: name,
            };
          });
        })
        .filter(Boolean)
        .sort((left, right) => left.label.localeCompare(right.label));
      const requestedDefault = String(
        document.querySelector("[data-world-native-chat]")
          ?.dataset.worldDefaultRepository || "forkmesh/forkmesh",
      );
      const previous =
        sessionStorage.getItem("forkmesh.worldChat.repository") ||
        requestedDefault;
      for (const repository of repositories.slice(0, 250)) {
        const option = document.createElement("option");
        option.value = repository.value;
        option.textContent = repository.label;
        option.dataset.logicalKind = repository.kind;
        option.dataset.routeOwner = repository.routeOwner;
        option.dataset.routeName = repository.routeName;
        fullRepository.append(option);
      }
      if (repositories.some((repository) => repository.value === previous)) {
        fullRepository.value = previous;
      }
      syncChatContextBubbles();
    } catch (_) {
      setComposerStatus("Repository catalog unavailable", "bad");
    }
  }

  async function runFullComposerAction(inputEl, attachmentControl = null) {
    const action = String(fullAction?.value || "chat");
    const text = String(inputEl?.value || "").trim();
    if (!text) {
      setComposerStatus("Write something first.", "bad");
      inputEl?.focus();
      return;
    }
    const repository = selectedComposerRepository();
    const taskAssigneeValue = String(taskAssignee?.value || "unassigned");
    const taskNeedsRepository =
      action === "task" &&
      (
        String(taskDestination?.value || "") === "repository" ||
        AGENT_ASSIGNEE_VALUES.includes(taskAssigneeValue)
      );
    if (!repository && (action !== "task" || taskNeedsRepository)) {
      setComposerStatus("Choose a repository for this action.", "bad");
      fullRepository?.focus();
      return;
    }
    if (
      action === "task" &&
      !simpleWorldComposer &&
      !String(taskTeam?.value || "")
    ) {
      setComposerStatus("Choose the team responsible for this task.", "bad");
      taskTeam?.focus();
      return;
    }
    setFullComposerBusy(true);
    setComposerStatus(
      action === "issue"
        ? "Signing issue…"
        : action === "task"
          ? "Saving private task…"
          : "Assigning agent…",
    );
    try {
      if (action === "task") {
        const lines = text.split(/\r?\n/);
        const title = String(lines.shift() || "").trim().slice(0, 160);
        const details = lines.join("\n").trim().slice(0, 4000);
        if (!title) throw new Error("Task title is required.");
        const botTask = AGENT_ASSIGNEE_VALUES.includes(taskAssigneeValue);
        const userAssignee = taskAssigneeValue.startsWith("user:")
          ? taskAssigneeValue.slice(5)
          : "";
        const destination = botTask
          ? "agent"
          : String(taskDestination?.value || "department");
        const taskAttachments = await Promise.all(
          (attachmentControl?.draft || []).map(({ file }) =>
            taskAttachmentMetadata(file),
          ),
        );
        const created = await taskApiRequest("POST", "/api/tasks", {
          title,
          details,
          department: String(taskDepartment?.value || "general"),
          team: String(taskTeam?.value || ""),
          destination,
          assigneeKind: botTask
            ? "agent"
            : userAssignee
              ? "user"
              : "unassigned",
          assignee: userAssignee,
          repository: repository
            ? `${repository.logicalOwner}/${repository.logicalName}`
            : "",
          attachments: taskAttachments,
          ...(destination === "qa"
            ? {
                howToTest:
                  details ||
                  "Follow the normal user flow and confirm the requested behavior.",
              }
            : {}),
        });
        const task = created?.task || {};
        // Bot work remains in the shared organization queue. MCP workers claim
        // it there, so the browser never creates a second mirror-local job.
        appendSystem(
          `Private task “${title}” was sent to ${
            destination === "qa"
              ? "the QA board"
              : destination === "agent"
                ? "the bot queue"
                : destination === "repository"
                  ? `${repository.owner}/${repository.name}`
                  : `${task.department || "general"}`
          }.`,
        );
        if (taskAttachments.length) clearDashboardDraft(attachmentControl);
        setComposerStatus("Organization task created.", "good");
      } else if (action === "issue") {
        const lines = text.split(/\r?\n/);
        const title = String(lines.shift() || "").trim().slice(0, 200);
        const body = lines.join("\n").trim();
        if (!title) throw new Error("Issue title is required.");
        const filed = await fileWebIssue(repository, title, body);
        appendSystem((await issueFiling()).issueFiledSummary(
          `${repository.owner}/${repository.name}`, title, filed));
        setComposerStatus(
          Number(filed?.number) > 0
            ? `Issue #${Number(filed.number)} created.`
            : "Issue sent to the maintainer inbox.",
          "good");
      } else {
        const lines = text.split(/\r?\n/);
        const title = String(lines.shift() || "").trim().slice(0, 160);
        const details = lines.join("\n").trim().slice(0, 4000);
        const taskAttachments = await Promise.all(
          (attachmentControl?.draft || []).map(({ file }) =>
            taskAttachmentMetadata(file),
          ),
        );
        const created = await taskApiRequest("POST", "/api/tasks", {
          title,
          details,
          department: "engineering",
          team: "",
          destination: "agent",
          assigneeKind: "agent",
          assignee: "",
          repository: `${repository.logicalOwner}/${repository.logicalName}`,
          attachments: taskAttachments,
        });
        const task = created?.task || {};
        const mention = "@bot";
        appendMessage(
          "self",
          displayName(),
          `${mention} ${text}`,
          "",
          "",
          Date.now(),
        );
        // MCP bots consume repository-linked agent tasks directly from the
        // organization queue; do not duplicate them in a mirror-local queue.
        if (taskAttachments.length) clearDashboardDraft(attachmentControl);
        appendSystem(
          `Private task “${title}” was added to the MCP task queue.`,
        );
        setComposerStatus("MCP task created.", "good");
      }
      inputEl.value = "";
      inputEl.style.height = "";
    } catch (error) {
      setComposerStatus(
        String(error?.message || "The action could not be completed."),
        "bad",
      );
    } finally {
      setFullComposerBusy(false);
      inputEl?.focus();
    }
  }

  async function runIssueSlashCommand(inputEl, attachmentControl, argument) {
    const text = String(argument || "").trim();
    if (!text) {
      setComposerStatus("Usage: /issue [owner/repo] <title>", "bad");
      return;
    }
    if (attachmentControl?.draft.length) {
      setComposerStatus(
        "Staged attachments are not included in /issue. Remove them or use Create repository issue.",
        "bad",
      );
      return;
    }
    const firstSpace = text.search(/\s/);
    const firstToken = firstSpace < 0 ? text : text.slice(0, firstSpace);
    const explicitTarget = /^[a-z][a-z0-9-]{0,62}\/[A-Za-z0-9._-]{1,100}$/.test(
      firstToken,
    );
    let repository = selectedComposerRepository();
    if (explicitTarget) {
      const option = Array.from(fullRepository?.options || []).find(
        (candidate) => candidate.value.toLowerCase() === firstToken.toLowerCase(),
      );
      if (!option) {
        setComposerStatus(`Repository ${firstToken} is not available.`, "bad");
        return;
      }
      fullRepository.value = option.value;
      repository = selectedComposerRepository();
    }
    if (!repository) {
      setComposerStatus("Choose a repository for this issue.", "bad");
      fullRepository?.focus();
      return;
    }
    const issueText = explicitTarget ? text.slice(firstToken.length).trim() : text;
    const title = String(issueText.split(/\r?\n/)[0] || "").trim().slice(0, 200);
    if (!title) {
      setComposerStatus("Usage: /issue [owner/repo] <title>", "bad");
      return;
    }
    setFullComposerBusy(true);
    setComposerStatus("Signing issue…");
    try {
      const filing = await issueFiling();
      const filed = await fileWebIssue(
        repository,
        title,
        filing.issueBodyFromMessage({
          text: issueText,
          who: displayName(),
          tsMs: Date.now(),
          channelLabel: CHANNEL_LABEL,
        }),
      );
      inputEl.value = "";
      inputEl.style.height = "";
      appendSystem(filing.issueFiledSummary(
        `${repository.logicalOwner}/${repository.logicalName}`, title, filed));
      setComposerStatus(
        Number(filed?.number) > 0
          ? `Issue #${Number(filed.number)} created.`
          : "Issue sent to the maintainer inbox.",
        "good");
    } catch (error) {
      setComposerStatus(
        String(error?.message || "The issue could not be filed."),
        "bad",
      );
    } finally {
      setFullComposerBusy(false);
      inputEl?.focus();
    }
  }

  let chatAppsLoad = null;

  function loadChatApps() {
    if (window.ForkMeshChatApps) return Promise.resolve(window.ForkMeshChatApps);
    if (!chatAppsLoad) {
      chatAppsLoad = new Promise((resolve) => {
        const script = document.createElement("script");
        script.src = "/chat-apps.js";
        script.onload = () => resolve(window.ForkMeshChatApps || null);
        script.onerror = () => resolve(null);
        document.head.appendChild(script);
      });
    }
    return chatAppsLoad;
  }

  async function runAppSlashCommand(inputEl, source) {
    const apps = await loadChatApps();
    if (!apps) return false;
    const handled = await apps.run(source, {
      channelId: "",
      showSystem: (text) => appendSystem(text),
      showEphemeral: (payload) => {
        if (!fullLog) return;
        clearEmptyState();
        fullLog.append(apps.ephemeralNode(payload, { channelId: "" }));
        pinFullLogToNewest();
      },
    });
    if (handled && inputEl) inputEl.value = "";
    return handled;
  }

  async function sendFrom(inputEl, attachmentControl) {
    const slash = String(inputEl?.value || "").trim().match(
      /^\/issue(?:\s+([\s\S]*))?$/i,
    );
    if (slash) {
      void runIssueSlashCommand(inputEl, attachmentControl, slash[1] || "");
      return;
    }
    const typed = String(inputEl?.value || "").trim();
    if (/^\/[a-z]/i.test(typed) && await runAppSlashCommand(inputEl, typed)) {
      return;
    }
    if (
      inputEl === fullInput &&
      fullAction &&
      fullAction.value !== "chat"
    ) {
      void runFullComposerAction(inputEl, attachmentControl);
      return;
    }
    if (!canJoinChat()) {
      showUserOnlyState();
      return;
    }
    const moderation = await moderateDashboardText(
      (inputEl?.value || "").trim());
    if (!moderation) {
      setStatus("Chat filter unavailable · message not sent");
      return;
    }
    const text = moderation.text;
    if (!text && !attachmentControl?.draft.length) return;
    closeMentionSuggest();
    if (text) {
      if (inputEl) inputEl.value = "";
      const isOrgAgentPrompt =
        CLAUDE_MENTION_RE.test(text) || CODEX_MENTION_RE.test(text);
      if (isOrgAgentPrompt) {
        if (attachmentControl?.draft.length) {
          showAttachmentFeedback(
            attachmentControl,
            "Agent chat attachments are not sent to the shared room. Add the relevant path in your prompt.",
          );
        }
        const clipped = text.slice(0, MAX_TEXT);
        const plain = makePlain("chat", { channel: CHANNEL, text: clipped });
        seen.add(plain.id);
        appendMessage("self", plain.sender, plain.text, plain.id, plain.senderId, plain.ts);
        emitWorldChatBubble(plain.sender, plain.senderId, plain.text);
        void maybeAskOrgAgent(clipped);
        if (moderation.changed) appendSystem("Some words were filtered.");
        return;
      }
      runWhenConnected(() => {
        const clipped = text.slice(0, MAX_TEXT);
        const plain = makePlain("chat", { channel: CHANNEL, text: clipped });
        send(plain);
        seen.add(plain.id);
        appendMessage("self", plain.sender, plain.text, plain.id, plain.senderId, plain.ts);
        emitWorldChatBubble(plain.sender, plain.senderId, plain.text);
        void maybeAskForkbot(clipped);
        if (moderation.changed) appendSystem("Some words were filtered.");
      });
    }
    void sendDashboardDraft(attachmentControl);
  }

  function wireInput(inputEl, sendEl) {
    if (!inputEl || !sendEl) return;
    inputEl.setAttribute("aria-autocomplete", "list");
    inputEl.setAttribute("aria-controls", "dashboard-chat-slash-suggest");
    inputEl.setAttribute("aria-expanded", "false");
    const attachmentControl = mountAttachmentControl(inputEl);
    sendEl.addEventListener("click", () => {
      if (simpleWorldComposer && inputEl === fullInput && fullAction) {
        fullAction.value = "chat";
        syncFullComposerAction();
      }
      if (simpleWorldComposer && inputEl === fullInput) {
        pulseWorldQuickComposer("chat");
      }
      sendFrom(inputEl, attachmentControl);
    });
    inputEl.addEventListener("paste", (event) => {
      const items = Array.from(event.clipboardData?.items || []);
      const item = items.find((candidate) =>
        candidate.kind === "file" && String(candidate.type || "").startsWith("image/"));
      const file = item ? item.getAsFile() : null;
      if (!file) return;
      event.preventDefault();
      void stageDashboardAttachments(attachmentControl, [file]).then(() => {
        if (
          inputEl !== fullInput ||
          String(fullAction?.value || "chat") === "chat"
        ) {
          return sendDashboardDraft(attachmentControl);
        }
        return null;
      });
    });
    inputEl.addEventListener("keydown", (event) => {
      if (slashSuggest && slashSuggest.input === inputEl) {
        if (event.key === "ArrowDown" || event.key === "ArrowUp") {
          event.preventDefault();
          moveSlashSuggest(event.key === "ArrowDown" ? 1 : -1);
          return;
        }
        if ((event.key === "Tab" || event.key === "Enter") && !event.shiftKey) {
          event.preventDefault();
          acceptSlashSuggest();
          return;
        }
        if (event.key === "Escape") {
          event.preventDefault();
          closeSlashSuggest();
          return;
        }
      }
      // While the mention list is open it owns Enter/Tab/arrows, so accepting a
      // name never sends the half-typed message.
      if (mentionSuggest && mentionSuggest.input === inputEl) {
        if (event.key === "ArrowDown") {
          event.preventDefault();
          moveMentionSuggest(1);
          return;
        }
        if (event.key === "ArrowUp") {
          event.preventDefault();
          moveMentionSuggest(-1);
          return;
        }
        if (event.key === "Tab" || event.key === "Enter") {
          event.preventDefault();
          acceptMentionSuggest();
          return;
        }
        if (event.key === "Escape") {
          event.preventDefault();
          closeMentionSuggest();
          return;
        }
      }
      if (event.key === "Enter" && !event.shiftKey) {
        event.preventDefault();
        if (simpleWorldComposer && inputEl === fullInput) {
          pulseWorldQuickComposer(fullAction?.value || "chat");
        }
        sendFrom(inputEl, attachmentControl);
      }
    });
    inputEl.addEventListener("input", () => updateSlashSuggest(inputEl));
    inputEl.addEventListener("input", () => updateMentionSuggest(inputEl));
    inputEl.addEventListener("input", () => {
      if (inputEl !== fullInput) return;
      if (fullInputManualHeight) {
        setFullInputManualHeight(
          Math.max(fullInputManualHeight, inputEl.scrollHeight),
        );
        return;
      }
      inputEl.style.height = "auto";
      inputEl.style.height = `${Math.min(inputEl.scrollHeight, 128)}px`;
    });
    inputEl.addEventListener("click", () => {
      updateSlashSuggest(inputEl);
      updateMentionSuggest(inputEl);
    });
    inputEl.addEventListener("keyup", (event) => {
      // Caret moves that leave the value alone (no "input" event) can still
      // enter or leave an "@token".
      if (["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) {
        updateSlashSuggest(inputEl);
        updateMentionSuggest(inputEl);
      }
    });
    inputEl.addEventListener("blur", closeSlashSuggest);
    inputEl.addEventListener("blur", closeMentionSuggest);
  }

  function wireTaskSend() {
    if (!fullTaskSend || !fullAction || !fullInput) return;
    fullTaskSend.addEventListener("click", () => {
      if (simpleWorldComposer) {
        // The compact World composer has no routing controls, so its task
        // action always creates an unassigned task on the General board.
        // Keep this separate from the dashboard's explicit bot-assignment
        // flow; clicking "Task" must never dispatch work to a bot.
        fullAction.value = "task";
        if (taskDepartment) taskDepartment.value = "general";
        if (taskTeam) taskTeam.value = "";
        if (taskDestination) taskDestination.value = "department";
        if (taskAssignee) taskAssignee.value = "unassigned";
        syncFullComposerAction();
        setComposerStatus("Creating an unassigned task in General…", "good");
        const attachmentControl = attachmentControls.find(
          (control) => control.inputEl === fullInput,
        );
        void runFullComposerAction(fullInput, attachmentControl);
        return;
      }
      if (fullAction.value !== "task") {
        fullAction.value = "task";
        syncFullComposerAction();
        setComposerStatus(
          "Choose a team, then assign this task to a person or an agent.",
        );
        taskDepartment?.focus();
        return;
      }
      void runFullComposerAction(fullInput);
    });
  }

  function directMessageStrip() {
    return document.querySelector("[data-dashboard-dm-strip]");
  }

  function renderDirectMessageStrip(conversations) {
    const strip = directMessageStrip();
    if (!strip) return;
    strip.textContent = "";
    const title = document.createElement("span");
    title.className = "font-semibold text-muted-foreground";
    title.textContent = "Direct messages";
    strip.append(title);
    for (const conversation of conversations.slice(0, 8)) {
      const name = String(conversation?.otherUser || "").trim();
      if (!name) continue;
      const chip = document.createElement("a");
      chip.href = "/chat?dm=" + encodeURIComponent(name);
      chip.className =
        "inline-flex items-center gap-1 rounded-full border border-border " +
        "px-2 py-0.5 font-semibold text-foreground hover:border-primary";
      chip.textContent = "@" + name;
      chip.title = `Open your direct messages with ${name}`;
      const unread = Number(conversation?.unreadCount) || 0;
      if (unread > 0) {
        const badge = document.createElement("span");
        badge.className =
          "rounded-full bg-primary px-1.5 text-[10px] font-bold " +
          "text-primary-foreground";
        badge.textContent = unread > 999 ? "999+" : String(unread);
        chip.append(badge);
      }
      strip.append(chip);
    }
    const start = document.createElement("a");
    start.href = "/chat?dm=";
    start.className =
      "inline-flex items-center rounded-full border border-border px-2 " +
      "py-0.5 font-semibold text-primary hover:border-primary";
    start.textContent = "+";
    start.setAttribute("aria-label", "Start a new direct message");
    start.title = "Start a new direct message";
    strip.append(start);
  }

  async function refreshDirectMessageStrip() {
    if (!directMessageStrip() || !userSession()) return;
    try {
      const response = await fetch("/api/chat/direct-messages?limit=20", {
        headers: discordRequestHeaders(),
        credentials: "same-origin",
        cache: "no-store",
      });
      if (!response.ok) return;
      const data = await response.json().catch(() => ({}));
      renderDirectMessageStrip(
        Array.isArray(data?.conversations) ? data.conversations : [],
      );
    } catch (_) {}
  }

  function mountDirectMessageStrip() {
    if (!PUBLIC_WORLD_GENERAL || !fullLog || WORLD_EMBED_BUBBLES) return;
    if (!userSession() || directMessageStrip()) return;
    const header =
      document.querySelector("[data-dashboard-chat-channel-header]") ||
      fullLog.previousElementSibling;
    if (!header || !header.parentElement) return;
    const strip = document.createElement("div");
    strip.dataset.dashboardDmStrip = "";
    strip.className =
      "flex flex-wrap items-center gap-2 border-b border-border/60 px-3 " +
      "py-1.5 text-xs";
    const railSlot = discordChatLayout()
      ? document.querySelector("[data-dashboard-chat-rail-dms]")
      : null;
    const wideRail = railSlot ? window.matchMedia?.("(min-width: 768px)") : null;
    const placeStrip = () => {
      if (railSlot && wideRail?.matches) railSlot.append(strip);
      else header.parentElement.insertBefore(strip, header.nextElementSibling);
    };
    placeStrip();
    wideRail?.addEventListener?.("change", placeStrip);
    renderDirectMessageStrip([]);
    window.addEventListener("forkmesh:direct-messages", () => {
      void refreshDirectMessageStrip();
    });
    void refreshDirectMessageStrip();
  }

  function mountPrivateChannelsLink() {
    if (!PUBLIC_WORLD_GENERAL || !fullLog) return;
    // The context rail and emote panel sit between the channel header and the
    // log, so the header must be found by name, not by adjacency; the sibling
    // fallback covers the World's simplified native-chat markup.
    const header =
      document.querySelector("[data-dashboard-chat-channel-header]") ||
      fullLog.previousElementSibling;
    if (!header || header.querySelector("[data-private-channels-link]")) return;
    const link = document.createElement("a");
    link.href = "/chat";
    link.textContent = "Open private channels";
    link.dataset.privateChannelsLink = "";
    link.className =
      "ml-auto text-xs font-semibold text-primary underline underline-offset-2 " +
      "hover:text-foreground";
    const status = header.querySelector("[data-dashboard-chat-status]")?.parentElement;
    header.insertBefore(link, status || null);
  }

  function discordChatLayout() {
    return Boolean(chatRail) && !WORLD_EMBED_BUBBLES && !simpleWorldComposer;
  }

  function chatDayStart(value) {
    const date = new Date(value);
    return new Date(date.getFullYear(), date.getMonth(), date.getDate()).getTime();
  }

  function fmtChatClock(tsMs) {
    const value = Number(tsMs);
    if (!value) return "";
    return new Date(value).toLocaleTimeString("en-US", {
      hour: "numeric", minute: "2-digit", hour12: true,
    });
  }

  function fmtChatStamp(tsMs) {
    const value = Number(tsMs);
    if (!value) return "";
    const days = Math.round((chatDayStart(Date.now()) - chatDayStart(value)) / 86400000);
    const clock = fmtChatClock(value);
    if (days === 0) return clock;
    if (days === 1) return `Yesterday at ${clock}`;
    const date = new Date(value).toLocaleDateString("en-US", {
      month: "numeric", day: "numeric", year: "2-digit",
    });
    return `${date}, ${clock}`;
  }

  function fmtChatFullDate(tsMs) {
    const value = Number(tsMs);
    if (!value) return "";
    return new Date(value).toLocaleString("en-US", {
      weekday: "long", month: "long", day: "numeric", year: "numeric",
      hour: "numeric", minute: "2-digit",
    });
  }

  function chatDayKey(tsMs) {
    const date = new Date(Number(tsMs) || 0);
    return `${date.getFullYear()}-${date.getMonth() + 1}-${date.getDate()}`;
  }

  function scheduleFullLogRegroup() {
    if (fullLogRegroupPending || !discordChatLayout()) return;
    fullLogRegroupPending = true;
    requestAnimationFrame(() => {
      fullLogRegroupPending = false;
      regroupFullLog();
    });
  }

  function regroupFullLog() {
    if (!fullLog) return;
    const kept = new Set();
    let previous = null;
    for (const element of Array.from(fullLog.children)) {
      if (element.hasAttribute("data-chat-day-divider")) continue;
      if (!element.classList.contains("chat-message-row")) {
        if (!element.hidden) previous = null;
        continue;
      }
      if (element.hidden) continue;
      const ts = Number(element.dataset.chatTimestamp) || 0;
      const day = chatDayKey(ts);
      if (!previous || previous.day !== day) {
        let divider = element.previousElementSibling;
        if (divider?.dataset.chatDayDivider !== day || kept.has(divider)) {
          divider = document.createElement("div");
          divider.className = "chat-day-divider";
          divider.dataset.chatDayDivider = day;
          divider.setAttribute("role", "separator");
          const label = document.createElement("span");
          label.textContent = new Date(ts).toLocaleDateString("en-US", {
            month: "long", day: "numeric", year: "numeric",
          });
          divider.append(label);
          fullLog.insertBefore(divider, element);
        }
        kept.add(divider);
      }
      const sender = element.dataset.chatSender || "";
      const grouped = Boolean(
        previous &&
        previous.day === day &&
        previous.sender === sender &&
        ts >= previous.ts &&
        ts - previous.ts < CHAT_GROUP_WINDOW_MS,
      );
      element.toggleAttribute("data-chat-grouped", grouped);
      previous = { day, sender, ts };
    }
    for (const divider of Array.from(
      fullLog.querySelectorAll(":scope > [data-chat-day-divider]"),
    )) {
      if (!kept.has(divider)) divider.remove();
    }
    if (!fullLog.children.length) ensureEmptyState();
  }

  function syncChatUserPanel(online = chatUserOnline) {
    chatUserOnline = Boolean(online);
    if (!chatRail) return;
    const name = displayName();
    const nameEl = chatRail.querySelector("[data-dashboard-chat-user-name]");
    if (nameEl) nameEl.textContent = name;
    const stateEl = chatRail.querySelector("[data-dashboard-chat-user-state]");
    if (stateEl) stateEl.textContent = chatUserOnline ? "Online" : "Offline";
    chatRail
      .querySelector("[data-dashboard-chat-user-presence]")
      ?.setAttribute("data-online", String(chatUserOnline));
    const avatar = chatRail.querySelector("[data-dashboard-chat-user-avatar]");
    if (avatar && name !== chatUserPanelName) {
      chatUserPanelName = name;
      hydrateChatAvatar(avatar, name);
    }
    scheduleMemberList();
  }

  function scheduleMemberList() {
    if (!memberPane || memberListPending) return;
    memberListPending = true;
    requestAnimationFrame(() => {
      memberListPending = false;
      renderMemberList();
    });
  }

  function memberListEntry(person, online, fetchAvatar) {
    const item = document.createElement("li");
    const linkable =
      person.kind === "user" && MENTIONABLE_NAME_RE.test(person.name);
    const row = document.createElement(linkable ? "a" : "div");
    if (linkable) row.href = mentionProfilePath(person.name);
    row.dataset.dashboardChatMember = person.name;
    const wrap = document.createElement("span");
    wrap.dataset.dashboardChatMemberAvatarWrap = "";
    const avatar = document.createElement("span");
    avatar.dataset.dashboardChatMemberAvatar = "";
    avatar.setAttribute("aria-hidden", "true");
    if (fetchAvatar) {
      hydrateChatAvatar(avatar, person.name);
    } else {
      avatar.textContent =
        person.kind === "bot" ? "🤖" : generatedChatFace(person.name);
    }
    wrap.append(avatar);
    if (online) {
      const presence = document.createElement("span");
      presence.dataset.dashboardChatMemberPresence = "";
      presence.dataset.online = "true";
      wrap.append(presence);
    }
    const label = document.createElement("span");
    label.dataset.dashboardChatMemberName = "";
    label.textContent = person.name;
    const text = document.createElement("span");
    text.dataset.dashboardChatMemberText = "";
    text.append(label);
    row.append(wrap, text);
    if (linkable && !isAutomatedMemberName(person.name)) {
      const stats = document.createElement("span");
      stats.dataset.dashboardChatMemberStats = "";
      stats.hidden = true;
      text.append(stats);
      const cached = freshMemberStats(person.name);
      if (cached) {
        fillMemberStats(stats, cached.entry);
      } else if (memberStatsObserver) {
        memberStatsObserver.observe(row);
      } else {
        wantMemberStats(person.name);
      }
    }
    if (person.kind === "bot") {
      const tag = document.createElement("span");
      tag.dataset.dashboardChatMemberTag = "";
      tag.textContent = "BOT";
      row.append(tag);
    }
    item.append(row);
    return item;
  }

  function isAutomatedMemberName(name) {
    return [
      FORKBOT_SENDER_ID, CLAUDE_SENDER_ID, CODEX_SENDER_ID, ORG_BOT_SENDER_ID,
    ].includes(name) || /(?:^|-)(?:bot|agent|mirror)$/.test(name);
  }

  function freshMemberStats(name) {
    const hit = memberStats.get(name);
    return hit && hit.expires > Date.now() ? hit : null;
  }

  function compactMemberCount(value) {
    const count = Math.max(0, Number(value) || 0);
    if (count < 1000) return String(count);
    if (count < 10000) return `${(Math.floor(count / 100) / 10).toFixed(1).replace(/\.0$/, "")}k`;
    return `${Math.floor(count / 1000)}k`;
  }

  function memberStatsAge(day) {
    const then = Date.parse(`${day}T00:00:00Z`);
    if (!Number.isFinite(then)) return null;
    const today = Date.parse(`${new Date().toISOString().slice(0, 10)}T00:00:00Z`);
    const days = Math.max(0, Math.round((today - then) / 86400000));
    if (days === 0) return { short: "today", long: "today" };
    if (days === 1) return { short: "1d", long: "yesterday" };
    if (days < 30) return { short: `${days}d`, long: `${days} days ago` };
    const months = Math.max(1, Math.floor(days / 30));
    return {
      short: `${months}mo`,
      long: months === 1 ? "a month ago" : `${months} months ago`,
    };
  }

  function memberStatsIcon(kind) {
    return `<svg aria-hidden="true" viewBox="0 0 24 24" width="11" height="11" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">${MEMBER_STATS_ICONS[kind]}</svg>`;
  }

  function fillMemberStats(stats, entry) {
    if (!stats) return;
    const parts = [];
    const labels = [];
    for (const kind of ["pulls", "commits", "issues", "reviews"]) {
      const count = Math.max(0, Number(entry?.[kind]) || 0);
      if (!count) continue;
      parts.push(`<span data-member-stat="${kind}">${memberStatsIcon(kind)}${escapeHtml(compactMemberCount(count))}</span>`);
      labels.push(`${count.toLocaleString()} ${MEMBER_STATS_LABELS[kind][count === 1 ? 0 : 1]}`);
    }
    const age = entry?.lastActive ? memberStatsAge(entry.lastActive) : null;
    if (age) {
      parts.push(`<span data-member-stat="recent">${memberStatsIcon("recent")}${escapeHtml(age.short)}</span>`);
      const activeDays = Math.max(0, Number(entry.activeDays) || 0);
      if (activeDays) {
        labels.push(`active on ${activeDays.toLocaleString()} ${activeDays === 1 ? "day" : "days"}`);
      }
      labels.push(`last contribution ${age.long}`);
    }
    if (!parts.length) {
      stats.hidden = true;
      stats.replaceChildren();
      stats.removeAttribute("title");
      return;
    }
    const summary = `Past year, public repositories: ${labels.join(", ")}`;
    stats.innerHTML = `<span data-member-stats-visible aria-hidden="true">${parts.join("")}</span><span data-member-stats-label>${escapeHtml(summary)}</span>`;
    stats.title = summary;
    stats.hidden = false;
  }

  function applyMemberStats(name) {
    if (!memberPane) return;
    const hit = freshMemberStats(name);
    if (!hit) return;
    for (const row of memberPane.querySelectorAll("[data-dashboard-chat-member]")) {
      if (row.dataset.dashboardChatMember !== name) continue;
      fillMemberStats(row.querySelector("[data-dashboard-chat-member-stats]"), hit.entry);
    }
  }

  function wantMemberStats(name) {
    if (!name || freshMemberStats(name)) return;
    memberStatsWanted.add(name);
    if (memberStatsTimer) return;
    memberStatsTimer = window.setTimeout(flushMemberStats, MEMBER_STATS_DELAY_MS);
  }

  async function flushMemberStats() {
    memberStatsTimer = 0;
    if (memberStatsInflight) return;
    const names = [...memberStatsWanted]
      .filter((name) => !freshMemberStats(name))
      .slice(0, MEMBER_STATS_BATCH)
      .sort();
    for (const name of names) memberStatsWanted.delete(name);
    if (!names.length) {
      memberStatsWanted.clear();
      return;
    }
    memberStatsInflight = true;
    let entries = null;
    try {
      const response = await fetch(
        `${MEMBER_STATS_ENDPOINT}?names=${encodeURIComponent(names.join(","))}`,
        { headers: { accept: "application/json" } },
      );
      const data = response.ok ? await response.json() : null;
      if (data && Array.isArray(data.members)) entries = data.members;
    } catch (_) {}
    const now = Date.now();
    const byName = new Map();
    for (const entry of entries || []) {
      const key = mentionName(entry?.name);
      if (key) byName.set(key, entry);
    }
    for (const name of names) {
      memberStats.set(name, {
        entry: byName.get(name) || null,
        expires: now + (entries ? MEMBER_STATS_TTL_MS : MEMBER_STATS_RETRY_MS),
      });
      applyMemberStats(name);
    }
    memberStatsInflight = false;
    if (memberStatsWanted.size && !memberStatsTimer) {
      memberStatsTimer = window.setTimeout(flushMemberStats, MEMBER_STATS_DELAY_MS);
    }
  }

  function watchMemberStats() {
    if (memberStatsObserver || !memberPane || !("IntersectionObserver" in window)) return;
    memberStatsObserver = new IntersectionObserver((records) => {
      for (const record of records) {
        if (!record.isIntersecting) continue;
        memberStatsObserver.unobserve(record.target);
        wantMemberStats(record.target.dataset.dashboardChatMember);
      }
    }, { root: memberPane, rootMargin: "160px 0px" });
  }

  function renderMemberList() {
    if (!memberPane || memberPane.offsetParent === null) return;
    watchMemberStats();
    memberStatsObserver?.disconnect();
    const selfName = displayName();
    const selfKey = mentionName(selfName);
    const byName = new Map();
    for (const person of mentionDirectory.values()) byName.set(person.name, person);
    for (const person of mentionPeople.values()) byName.set(person.name, person);
    byName.set(FORKBOT_SENDER_ID, {
      name: FORKBOT_SENDER_ID,
      kind: "bot",
      lastSeenMs: Date.now(),
    });
    byName.delete(selfKey);
    const online = [];
    const offline = [];
    for (const person of byName.values()) {
      (mentionIsOnline(person) ? online : offline).push(person);
    }
    const byLabel = (left, right) => left.name.localeCompare(right.name);
    online.sort(byLabel);
    offline.sort(byLabel);
    if (chatUserOnline) {
      online.unshift({
        name: selfName,
        kind: userSession() ? "user" : "guest",
        lastSeenMs: Date.now(),
      });
    }
    const groups = { online, offline };
    for (const [key, people] of Object.entries(groups)) {
      const heading = memberPane.querySelector(
        `[data-dashboard-chat-members-heading="${key}"]`,
      );
      const list = memberPane.querySelector(
        `[data-dashboard-chat-members-list="${key}"]`,
      );
      if (heading) {
        heading.textContent =
          `${key === "online" ? "Online" : "Offline"} — ${people.length}`;
      }
      if (!list) continue;
      list.replaceChildren(
        ...people.slice(0, MEMBER_LIST_LIMIT).map((person, index) =>
          memberListEntry(
            person,
            key === "online",
            key === "online" || index < MEMBER_AVATAR_FETCH_LIMIT,
          )),
      );
    }
  }

  function memberListWide() {
    return Boolean(window.matchMedia?.("(min-width: 1200px)").matches);
  }

  function syncMemberListToggle(toggle) {
    if (!chatView || !toggle) return;
    const shown = memberListWide()
      ? !chatView.hasAttribute("data-members-hidden")
      : chatView.hasAttribute("data-members-open");
    const label = shown ? "Hide member list" : "Show member list";
    toggle.setAttribute("aria-pressed", String(shown));
    toggle.setAttribute("aria-label", label);
    toggle.title = label;
  }

  function setMemberListHidden(hidden, toggle) {
    if (!chatView) return;
    chatView.toggleAttribute("data-members-hidden", hidden);
    syncMemberListToggle(toggle);
    if (!hidden && memberListWide()) {
      void refreshMentionDirectory();
    }
    scheduleMemberList();
  }

  function setMemberDrawerOpen(open, toggle) {
    if (!chatView) return;
    chatView.toggleAttribute("data-members-open", open);
    syncMemberListToggle(toggle);
    if (open) void refreshMentionDirectory();
    scheduleMemberList();
  }

  function closeComposerEmojiPicker({ restoreFocus = false } = {}) {
    if (!composerEmojiPicker) return;
    const { element, trigger } = composerEmojiPicker;
    element.remove();
    composerEmojiPicker = null;
    trigger.setAttribute("aria-expanded", "false");
    if (restoreFocus) trigger.focus();
  }

  function insertComposerEmoji(emoji) {
    if (!fullInput || fullInput.disabled) return;
    const start = fullInput.selectionStart ?? fullInput.value.length;
    const end = fullInput.selectionEnd ?? start;
    fullInput.setRangeText(emoji, start, end, "end");
    fullInput.dispatchEvent(new Event("input", { bubbles: true }));
    fullInput.focus();
  }

  function openComposerEmojiPicker(trigger) {
    const host = trigger.closest("[data-dashboard-chat-compose-row]");
    if (!host) return;
    const picker = document.createElement("div");
    picker.className = "chat-reaction-picker";
    picker.dataset.dashboardChatEmojiPicker = "";
    picker.setAttribute("role", "toolbar");
    picker.setAttribute("aria-label", "Choose an emoji");
    for (const emoji of COMPOSER_EMOJI) {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = emoji;
      button.setAttribute("aria-label", `Insert ${emoji}`);
      button.addEventListener("click", () => {
        closeComposerEmojiPicker();
        insertComposerEmoji(emoji);
      });
      picker.append(button);
    }
    picker.addEventListener("keydown", (event) => {
      if (event.key !== "Escape") return;
      event.preventDefault();
      closeComposerEmojiPicker({ restoreFocus: true });
    });
    host.append(picker);
    composerEmojiPicker = { element: picker, trigger };
    trigger.setAttribute("aria-expanded", "true");
    picker.querySelector("button")?.focus();
  }

  function syncDiscordChrome() {
    if (!chatView || !discordChatLayout()) return;
    const channelName = PUBLIC_WORLD_GENERAL
      ? "World"
      : String(CHANNEL || "#general").replace(/^#/, "");
    const nameEl = chatView.querySelector("[data-dashboard-chat-channel-name]");
    if (nameEl) nameEl.textContent = channelName;
    const worldLabel = chatRail?.querySelector(
      '[data-dashboard-chat-channel-link=""] [data-dashboard-chat-channel-label]');
    if (worldLabel) worldLabel.textContent = "World";
    const topic = chatView.querySelector("[data-dashboard-chat-topic]");
    if (topic) {
      topic.textContent = PUBLIC_WORLD_GENERAL
        ? "Public World chat · ForkBot and MCP can read and reply here"
        : `Dedicated ${CHANNEL} channel inside the encrypted ForkMesh room`;
    }
    fullInput?.setAttribute("aria-label", `Message ${CHANNEL}`);
    chatRail
      ?.querySelectorAll("[data-dashboard-chat-channel-link]")
      .forEach((link) => {
        const current =
          !requestedOrganization &&
          link.dataset.dashboardChatChannelLink === ACTIVE_SPACE;
        if (current) link.setAttribute("aria-current", "page");
        else link.removeAttribute("aria-current");
      });
    syncChatUserPanel();
  }

  function mountDiscordChrome() {
    if (!chatView || !discordChatLayout()) return;
    if (fullInput) fullInput.rows = 1;
    syncDiscordChrome();
    if (fullLog && "MutationObserver" in window) {
      new MutationObserver(scheduleFullLogRegroup)
        .observe(fullLog, { childList: true });
      new MutationObserver(scheduleFullLogRegroup).observe(fullLog, {
        subtree: true,
        attributes: true,
        attributeFilter: ["hidden"],
      });
    }
    scheduleFullLogRegroup();
    chatRail
      .querySelectorAll("[data-dashboard-chat-channel-link]")
      .forEach((link) => {
        link.addEventListener("click", (event) => {
          if (
            event.defaultPrevented || event.button !== 0 ||
            event.metaKey || event.ctrlKey || event.shiftKey || event.altKey
          ) {
            return;
          }
          const space = link.dataset.dashboardChatChannelLink || "";
          if (!switchChatSpace(space)) return;
          event.preventDefault();
          const destination = new URL(location.href);
          if (space) destination.searchParams.set("space", space);
          else destination.searchParams.delete("space");
          history.replaceState(
            history.state,
            "",
            `${destination.pathname}${destination.search}${destination.hash}`,
          );
        });
      });
    const sectionToggle = chatRail.querySelector(
      "[data-dashboard-chat-section-toggle]",
    );
    sectionToggle?.addEventListener("click", () => {
      const section = sectionToggle.closest("[data-dashboard-chat-rail-section]");
      if (!section) return;
      const collapsed = !section.hasAttribute("data-collapsed");
      section.toggleAttribute("data-collapsed", collapsed);
      sectionToggle.setAttribute("aria-expanded", String(!collapsed));
    });
    const membersToggle = chatView.querySelector(
      "[data-dashboard-chat-members-toggle]",
    );
    let membersHidden = false;
    try {
      membersHidden = localStorage.getItem(MEMBER_LIST_HIDDEN_KEY) === "1";
    } catch (_) {}
    const wideMembers = window.matchMedia?.("(min-width: 1200px)");
    setMemberListHidden(membersHidden, membersToggle);
    membersToggle?.addEventListener("click", () => {
      if (!memberListWide()) {
        setMemberDrawerOpen(!chatView.hasAttribute("data-members-open"), membersToggle);
        return;
      }
      const hidden = !chatView.hasAttribute("data-members-hidden");
      try {
        localStorage.setItem(MEMBER_LIST_HIDDEN_KEY, hidden ? "1" : "0");
      } catch (_) {}
      setMemberListHidden(hidden, membersToggle);
    });
    chatView.addEventListener("keydown", (event) => {
      if (event.key !== "Escape" || !chatView.hasAttribute("data-members-open")) return;
      setMemberDrawerOpen(false, membersToggle);
      membersToggle?.focus();
    });
    chatView.addEventListener("pointerdown", (event) => {
      if (!chatView.hasAttribute("data-members-open") || memberListWide()) return;
      if (memberPane?.contains(event.target) || membersToggle?.contains(event.target)) return;
      setMemberDrawerOpen(false, membersToggle);
    });
    wideMembers?.addEventListener?.("change", () => {
      if (wideMembers.matches) chatView.removeAttribute("data-members-open");
      syncMemberListToggle(membersToggle);
      if (wideMembers.matches && !chatView.hasAttribute("data-members-hidden")) {
        void refreshMentionDirectory();
      }
      scheduleMemberList();
    });
    window.setInterval(scheduleMemberList, 60000);
    const emojiToggle = chatView.querySelector(
      "[data-dashboard-chat-emoji-toggle]",
    );
    emojiToggle?.addEventListener("click", () => {
      if (composerEmojiPicker) closeComposerEmojiPicker();
      else openComposerEmojiPicker(emojiToggle);
    });
    document.addEventListener("pointerdown", (event) => {
      if (!composerEmojiPicker) return;
      const { element, trigger } = composerEmojiPicker;
      if (element.contains(event.target) || trigger.contains(event.target)) return;
      closeComposerEmojiPicker();
    });
  }

  async function initChat() {
    if (ACTIVE_SPACE) {
      document.title = `${CHANNEL_LABEL} collaboration · ForkMesh`;
      document
        .querySelectorAll("[data-dashboard-chat-status]")
        .forEach((element) => {
          element.title = `Dedicated ${CHANNEL} channel inside the encrypted ForkMesh room`;
        });
    }
    await hydrateUserSession();
    await loadOrgAgentChatAccess();
    ensureEmptyState();
    mountPrivateChannelsLink();
    mountDirectMessageStrip();
    mountDiscordChrome();
    if (fullChannel) {
      fullChannel.value = ACTIVE_SPACE || "";
      fullChannel.addEventListener("change", () => {
        const destination = new URL(location.href);
        destination.searchParams.set("worldEmbed", "1");
        if (fullChannel.value) {
          destination.searchParams.set("space", fullChannel.value);
        } else {
          destination.searchParams.delete("space");
        }
        destination.searchParams.delete("org");
        destination.searchParams.delete("repo");
        destination.searchParams.delete("run");
        location.assign(`${destination.pathname}${destination.search}`);
      });
    }
    fullRepository?.addEventListener("change", () => {
      try {
        sessionStorage.setItem(
          "forkmesh.worldChat.repository",
          fullRepository.value,
        );
      } catch (_) {}
      syncChatContextBubbles();
    });
    fullAction?.addEventListener("change", syncFullComposerAction);
    taskDestination?.addEventListener("change", syncFullComposerAction);
    taskAssignee?.addEventListener("change", syncFullComposerAction);
    syncFullComposerAction();
    contextChannel?.addEventListener("click", () => fullChannel?.focus());
    contextSource?.addEventListener("click", () => fullRepository?.focus());
    contextAction?.addEventListener("click", () => fullAction?.focus());
    document
      .querySelectorAll("[data-dashboard-chat-scroll]")
      .forEach((button) => {
        button.addEventListener("click", () => {
          if (!fullLog) return;
          const direction = button.dataset.dashboardChatScroll;
          if (
            direction === "up" &&
            fullLog.scrollTop <= historyPreloadThreshold() &&
            hasHiddenHistory()
          ) {
            revealOlderHistory({ fillBuffer: true });
            return;
          }
          fullLog.scrollTo({
            top:
              direction === "up"
                ? Math.max(0, fullLog.scrollTop - fullLog.clientHeight * 0.8)
                : fullLog.scrollHeight,
            behavior: "smooth",
          });
        });
      });
    fullLog?.addEventListener("scroll", () => {
      stickToNewest = fullLogAtNewest();
      const currentTop = fullLog.scrollTop;
      const revealOlder =
        hasHiddenHistory() &&
        currentTop <= historyPreloadThreshold() &&
        currentTop < lastFullLogScrollTop - 1 &&
        !historyWheelLatched;
      lastFullLogScrollTop = currentTop;
      syncChatScrollThumb();
      if (revealOlder) {
        historyWheelLatched = true;
        scheduleHistoryWheelReset();
        revealOlderHistory({ fillBuffer: true });
      }
    }, {
      passive: true,
    });
    fullLog?.addEventListener("wheel", (event) => {
      scheduleHistoryWheelReset();
      if (event.deltaY > 0) historyWheelLatched = false;
      if (
        event.deltaY < 0 &&
        hasHiddenHistory() &&
        fullLog.scrollTop <= historyPreloadThreshold() &&
        !historyWheelLatched
      ) {
        historyWheelLatched = true;
        revealOlderHistory({ fillBuffer: true });
      }
    }, { passive: true });
    fullLog?.addEventListener("touchstart", (event) => {
      historyTouchRevealed = false;
      historyTouchStartY =
        fullLog.scrollTop <= historyPreloadThreshold()
          ? Number(event.touches?.[0]?.clientY)
          : null;
    }, { passive: true });
    fullLog?.addEventListener("touchmove", (event) => {
      const currentY = Number(event.touches?.[0]?.clientY);
      if (
        Number.isFinite(historyTouchStartY) &&
        Number.isFinite(currentY) &&
        currentY - historyTouchStartY >= 28 &&
        !historyTouchRevealed &&
        hasHiddenHistory()
      ) {
        historyTouchRevealed = true;
        revealOlderHistory({ fillBuffer: true });
      }
    }, { passive: true });
    fullLog?.addEventListener("touchend", () => {
      historyTouchStartY = null;
      historyTouchRevealed = false;
    }, { passive: true });
    if (fullLog && "ResizeObserver" in window) {
      new ResizeObserver(() => {
        if (stickToNewest) pinFullLogToNewest();
        else scheduleChatScrollThumb();
      }).observe(fullLog);
    }
    scheduleChatScrollThumb();
    document
      .querySelectorAll("[data-dashboard-chat-emote]")
      .forEach((button) => {
        button.addEventListener("click", () => {
          const emote = String(button.dataset.dashboardChatEmote || "");
          dispatchWorldEmote(emote);
        });
      });
    void loadComposerRepositories();
    void loadTaskRouting();
    wireFullInputResizeHandle();
    wireInput(fullInput, fullSend);
    wireTaskSend();
    wireInput(sideInput, sideSend);
    // Connect right away so the room's message history (replayed by the relay
    // on WebSocket open) is visible without the visitor first focusing an input.
    if (canJoinChat()) {
      setStatus("Not connected");
      connect();
    }
    startDiscordMessageRefresh();
  }

  initChat();
  return true;
}

let dashboardChatController = null;
window.ForkMeshDashboardChat = Object.freeze({
  mount: mountForkMeshDashboardChat,
  switchSpace: (space) => Boolean(dashboardChatController?.switchSpace(space)),
  activeSpace: () => dashboardChatController?.activeSpace() ?? "",
  channel: () => dashboardChatController?.channel() ?? "#general",
  spaces: () => dashboardChatController?.spaces() ?? [],
});
mountForkMeshDashboardChat({ space: String(window.FORKMESH_CHAT_SPACE || "") });

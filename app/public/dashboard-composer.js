(() => {
  if (window.ForkMeshComposer) return;

  const MAX_PROMPT = 8000;
  const HISTORY_LIMIT = 50;
  const STORAGE_PREFIX = "forkmesh.composer.v1:";
  const RECENT_REPOS_PREFIX = "forkmesh.agent.recentRepositories.v1:";
  const AGENT_ATTACHMENT_MAX_COUNT = 5;
  const AGENT_ATTACHMENT_MAX_BYTES = 5 * 1024 * 1024;
  const AGENT_ATTACHMENT_MAX_TOTAL_BYTES = 10 * 1024 * 1024;
  const TASK_IMAGE_MAX_COUNT = 4;
  const TASK_IMAGE_MAX_BYTES = 256 * 1024;
  const TASK_IMAGE_MAX_TOTAL_BYTES = 1024 * 1024;
  const TASK_TITLE_MAX = 160;
  const TASK_DETAILS_MAX = 4000;
  const TEXT_FILE_MAX_BYTES = 64 * 1024;
  const IMAGE_TYPES = new Set(["image/jpeg", "image/png", "image/webp", "image/gif"]);
  const GAUGE_BARS = 5;
  const DEFAULT_PLACEHOLDER = "Describe a task or ask a question…";
  const SHORTCUT_LABEL = /Mac|iPhone|iPad/.test(navigator.platform || "") ? "⌘/" : "Ctrl+/";

  const AGENTS = [
    { value: "auto", label: "Auto", title: "Auto: the node runs the first agent it has installed" },
    { value: "claude-code", label: "Claude Code", title: "Run Claude Code" },
    { value: "codex", label: "Codex", title: "Run Codex" },
    { value: "local-llama", label: "Gemma", title: "Run Gemma on the node's own Ollama" },
  ];
  const RUN_MODES = [
    { value: "auto", label: "Auto", hint: "run without asking", icon: "mode-auto" },
    { value: "ask", label: "Ask", hint: "confirm every change", icon: "mode-ask" },
    { value: "plan", label: "Plan", hint: "read-only, propose a plan", icon: "mode-plan" },
    { value: "edit", label: "Edit", hint: "edit files, ask before commands", icon: "mode-edit" },
  ];
  const EFFORTS = [
    { value: "low", label: "Low", icon: "effort-low" },
    { value: "medium", label: "Medium", icon: "effort-medium" },
    { value: "high", label: "High", icon: "effort-high" },
    { value: "xhigh", label: "Ultra", icon: "effort-ultra" },
    { value: "max", label: "Max", icon: "effort-max" },
  ];
  const DEFAULT_RUN_MODE = "auto";
  const DEFAULT_EFFORT = "high";
  const MODEL_CATALOG = {
    "claude-code": [
      { value: "opus", label: "Opus 5.5", id: "claude-opus-5-5", icon: "model-opus-face" },
      { value: "opus-5", label: "Opus 5", id: "claude-opus-5", icon: "model-opus-face" },
      { value: "sonnet", label: "Sonnet 5", id: "claude-sonnet-5", icon: "model-sonnet-face" },
      { value: "haiku", label: "Haiku 4.5", id: "claude-haiku-4-5", icon: "model-haiku-face" },
      { value: "fable", label: "Fable 5", id: "claude-fable-5", icon: "model-fable-face" },
    ],
    codex: [
      { value: "sol", label: "GPT-5.6 Sol", id: "gpt-5.6-sol", icon: "model-sol-face" },
      { value: "terra", label: "GPT-5.6 Terra", id: "gpt-5.6-terra", icon: "model-terra-face" },
      { value: "luna", label: "GPT-5.6 Luna", id: "gpt-5.6-luna", icon: "model-luna-face" },
      { value: "spark", label: "Codex Spark", id: "gpt-5.3-codex-spark" },
    ],
    "local-llama": [
      { value: "gemma4:e4b", label: "Gemma 4 E4B", id: "gemma4:e4b" },
      { value: "gemma4:e2b", label: "Gemma 4 E2B", id: "gemma4:e2b" },
      { value: "gemma4:12b", label: "Gemma 4 12B", id: "gemma4:12b" },
      { value: "gemma4:26b", label: "Gemma 4 26B", id: "gemma4:26b" },
      { value: "gemma4:31b", label: "Gemma 4 31B", id: "gemma4:31b" },
    ],
  };
  const DEFAULT_MODEL = { "claude-code": "opus", codex: "sol", "local-llama": "gemma4:e4b" };
  const PROMPTABLE_STATUSES = new Set(["running", "queued", "completed", "failed"]);
  const CLOUD_RUNTIMES = new Set(["headless", "runner-pool"]);
  const RUNNER_SETUP_PAGE = "/dashboard/runners?setup=1";
  const DESKTOP_PAGE = "/desktop";
  const BUILTIN_COMMANDS = new Set(["clear", "apps", "help", "model", "mode", "agent", "speed"]);

  const ICONS = {
    x: '<path d="M18 6 6 18"/><path d="m6 6 12 12"/>',
    grip: '<circle cx="9" cy="6" r="1"/><circle cx="15" cy="6" r="1"/><circle cx="9" cy="12" r="1"/><circle cx="15" cy="12" r="1"/><circle cx="9" cy="18" r="1"/><circle cx="15" cy="18" r="1"/>',
    refresh: '<path d="M21 12a9 9 0 0 0-9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/><path d="M3 12a9 9 0 0 0 9 9 9.75 9.75 0 0 0 6.74-2.74L21 16"/><path d="M16 16h5v5"/>',
    expand: '<path d="M15 3h6v6"/><path d="M9 21H3v-6"/><path d="m21 3-7 7"/><path d="m3 21 7-7"/>',
    shrink: '<path d="m14 10 7-7"/><path d="M20 10h-6V4"/><path d="m3 21 7-7"/><path d="M4 14h6v6"/>',
    minimize: '<path d="m6 9 6 6 6-6"/>',
    task: '<path d="m3 17 2 2 4-4"/><path d="m3 7 2 2 4-4"/><path d="M13 6h8"/><path d="M13 12h8"/><path d="M13 18h8"/>',
    add: '<path d="m5 12 7-7 7 7"/><path d="M12 19V5"/>',
    send: '<path d="M14.536 21.686a.5.5 0 0 0 .937-.024l6.5-19a.496.496 0 0 0-.635-.635l-19 6.5a.5.5 0 0 0-.024.937l7.93 3.18a2 2 0 0 1 1.112 1.11z"/><path d="m21.854 2.147-10.94 10.939"/>',
    clip: '<path d="m21.44 11.05-9.19 9.19a6 6 0 0 1-8.49-8.49l8.57-8.57A4 4 0 1 1 18 8.84l-8.59 8.57a2 2 0 0 1-2.83-2.83l8.49-8.48"/>',
    mic: '<path d="M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3Z"/><path d="M19 10v2a7 7 0 0 1-14 0v-2"/><line x1="12" x2="12" y1="19" y2="22"/>',
    slash: '<path d="M22 2 2 22"/>',
    prompt: '<path d="M12 20h9"/><path d="M16.376 3.622a1 1 0 0 1 3.002 3.002L7.368 18.635a2 2 0 0 1-.855.506l-2.872.838a.5.5 0 0 1-.62-.62l.838-2.872a2 2 0 0 1 .506-.854z"/>',
    file: '<path d="M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7Z"/><path d="M14 2v4a2 2 0 0 0 2 2h4"/>',
    repo: '<path d="M4 19.5v-15A2.5 2.5 0 0 1 6.5 2H19a1 1 0 0 1 1 1v18a1 1 0 0 1-1 1H6.5a1 1 0 0 1 0-5H20"/>',
    issue: '<circle cx="12" cy="12" r="10"/><circle cx="12" cy="12" r="1"/>',
    pull: '<circle cx="18" cy="18" r="3"/><circle cx="6" cy="6" r="3"/><path d="M13 6h3a2 2 0 0 1 2 2v7"/><line x1="6" x2="6" y1="9" y2="21"/>',
    cloud: '<path d="M17.5 19H9a7 7 0 1 1 6.71-9h1.79a4.5 4.5 0 1 1 0 9Z"/>',
    chevron: '<path d="m6 9 6 6 6-6"/>',
    chevronRight: '<path d="m9 18 6-6-6-6"/>',
    check: '<path d="M20 6 9 17l-5-5"/>',
    zap: '<path d="M4 14a1 1 0 0 1-.78-1.63l9.9-10.2a.5.5 0 0 1 .86.46l-1.92 6.02A1 1 0 0 0 13 10h7a1 1 0 0 1 .78 1.63l-9.9 10.2a.5.5 0 0 1-.86-.46l1.92-6.02A1 1 0 0 0 11 14z"/>',
    ask: '<path d="M7.9 20A9 9 0 1 0 4 16.1L2 22Z"/><path d="M9.09 9a3 3 0 0 1 5.83 1c0 2-3 3-3 3"/><path d="M12 17h.01"/>',
    map: '<path d="M14.106 5.553a2 2 0 0 0 1.788 0l3.659-1.83A1 1 0 0 1 21 4.619v12.764a1 1 0 0 1-.553.894l-4.553 2.277a2 2 0 0 1-1.788 0l-4.212-2.106a2 2 0 0 0-1.788 0l-3.659 1.83A1 1 0 0 1 3 19.381V6.618a1 1 0 0 1 .553-.894l4.553-2.277a2 2 0 0 1 1.788 0z"/><path d="M15 5.764v15"/><path d="M9 3.236v15"/>',
    server: '<rect width="20" height="8" x="2" y="2" rx="2"/><rect width="20" height="8" x="2" y="14" rx="2"/><path d="M6 6h.01"/><path d="M6 18h.01"/>',
  };
  const MODE_GLYPHS = { auto: "zap", ask: "ask", plan: "map", edit: "prompt" };
  const EFFORT_ROW = "effort";

  function icon(name, extra = "") {
    return `<svg class="fmc-icon${extra ? " " + extra : ""}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" focusable="false">${ICONS[name] || ""}</svg>`;
  }

  function choiceGlyph(key, value) {
    return key === "mode" && MODE_GLYPHS[value] ? icon(MODE_GLYPHS[value], "fmc-choice-glyph") : "";
  }

  function escapeHtml(value) {
    return String(value ?? "").replace(/[&<>"']/g, (ch) => ({
      "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
    }[ch]));
  }

  function host() {
    return window.ForkMeshComposerHost || null;
  }

  function readSession() {
    try {
      const fromHost = host()?.session?.();
      if (fromHost) return fromHost;
    } catch (_) {}
    try {
      return JSON.parse(localStorage.getItem("forkmesh.session") || "null");
    } catch (_) {
      return null;
    }
  }

  function isUserSession(session) {
    return Boolean(session?.nodeName && session.kind !== "node");
  }

  function accountKey(session) {
    return String(session?.nodeName || session?.email || "").trim().toLowerCase();
  }

  function bearerToken(session) {
    const token = String(session?.sessionToken || "");
    return token && token !== "cookie" ? token : "";
  }

  function friendlyCode(code) {
    const text = String(code || "").replace(/_/g, " ").trim();
    return text ? text.charAt(0).toUpperCase() + text.slice(1) + "." : "";
  }

  async function api(method, path, body, withSessionToken = false) {
    const session = readSession();
    const token = bearerToken(session);
    const headers = { accept: "application/json" };
    if (token) headers.authorization = "Bearer " + token;
    const init = { method, headers, credentials: "same-origin", cache: "no-store" };
    if (body !== undefined) {
      headers["content-type"] = "application/json";
      init.body = JSON.stringify(withSessionToken && token ? { ...body, sessionToken: token } : body);
    }
    const response = await fetch(path, init);
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload?.ok === false) {
      const message = String(payload?.message || "").trim() ||
        friendlyCode(typeof payload?.error === "string" ? payload.error : "") ||
        `Request failed (HTTP ${response.status}).`;
      const error = new Error(message);
      error.status = response.status;
      error.payload = payload;
      throw error;
    }
    return payload;
  }

  function repoFromDisplay(display, session) {
    const [owner, name] = String(display || "").split("/");
    if (!owner || !name) return null;
    const me = String(session?.nodeName || "").toLowerCase();
    const nodes = Array.isArray(session?.nodes) ? session.nodes.map((n) => String(n || "").toLowerCase()) : [];
    const lowered = owner.toLowerCase();
    const owns = lowered === me || nodes.includes(lowered);
    const o = encodeURIComponent(owner);
    const n = encodeURIComponent(name);
    return {
      owner,
      name,
      key: `${owner}/${name}`,
      display: `${owner}/${name}`,
      owns,
      endpoint: owns ? `/api/repo/${o}/${n}/agent-sessions` : `/api/orgs/${o}/repos/${n}/agent-bots`,
    };
  }

  function parseRepoPath(pathname) {
    const parts = String(pathname || "").split("/").filter(Boolean);
    let decoded;
    try {
      decoded = parts.map((part) => decodeURIComponent(part));
    } catch (_) {
      return null;
    }
    if (decoded.length < 2 || decoded[0].startsWith("@")) return null;
    const result = { owner: decoded[0], name: decoded[1], kind: "repo", number: 0 };
    const tab = String(decoded[2] || "").toLowerCase();
    const number = /^[1-9][0-9]{0,8}$/.test(decoded[3] || "") ? Number(decoded[3]) : 0;
    if (number && tab === "issues") {
      result.kind = "issue";
      result.number = number;
    } else if (number && (tab === "pulls" || tab === "pull")) {
      result.kind = "pull";
      result.number = number;
    }
    return result;
  }

  function currentPage() {
    try {
      const page = host()?.page?.();
      if (page) return String(page);
    } catch (_) {}
    return String(document.body?.dataset?.page || "");
  }

  function detectContext(session) {
    if (currentPage() !== "repo") return null;
    const parsed = parseRepoPath(location.pathname);
    if (!parsed) return null;
    let repo = null;
    try {
      const hosted = host()?.repo?.();
      if (hosted && hosted.name.toLowerCase() === parsed.name.toLowerCase()) repo = hosted;
    } catch (_) {}
    if (!repo) repo = repoFromDisplay(`${parsed.owner}/${parsed.name}`, session);
    if (!repo) return null;
    const origin = location.origin;
    const segment = parsed.kind === "issue" ? "issues" : "pulls";
    const url = parsed.kind === "repo"
      ? `${origin}/${repo.display}`
      : `${origin}/${repo.display}/${segment}/${parsed.number}`;
    const label = parsed.kind === "repo"
      ? repo.display
      : `${repo.display} ${parsed.kind === "issue" ? "issue" : "PR"} #${parsed.number}`;
    return { repo, kind: parsed.kind, number: parsed.number, url, label, key: `${parsed.kind}:${url}` };
  }

  function contextSuffix(context) {
    if (!context || context.kind === "repo") return "";
    const noun = context.kind === "issue" ? "Issue" : "Pull request";
    return `\n\nContext: ${noun} #${context.number} in ${context.repo.display} (${context.url})`;
  }

  function splitTaskText(text) {
    const trimmed = String(text || "").trim();
    const newline = trimmed.indexOf("\n");
    const first = (newline >= 0 ? trimmed.slice(0, newline) : trimmed).trim();
    const rest = newline >= 0 ? trimmed.slice(newline + 1).trim() : "";
    if (first.length <= TASK_TITLE_MAX) return { title: first, details: rest };
    return { title: first.slice(0, TASK_TITLE_MAX - 1).trimEnd() + "…", details: trimmed };
  }

  function slashCommandName(text) {
    const match = String(text || "").trim().match(/^\/([a-z][a-z0-9-]{1,31})(?:\s|$)/i);
    return match ? match[1].toLowerCase() : "";
  }

  function modelOptions(mode, allowed) {
    const provider = modeProvider(mode);
    const list = MODEL_CATALOG[provider];
    const permitted = Array.isArray(allowed?.[provider]) && allowed[provider].length
      ? new Set(allowed[provider].map((value) => String(value).toLowerCase()))
      : null;
    return permitted ? list.filter((option) => permitted.has(option.value)) : list.slice();
  }

  function isCloudTarget(target) {
    return CLOUD_RUNTIMES.has(String(target?.runtimeMode || "").toLowerCase());
  }

  function eligibleTargets(targets, mode, cloud) {
    const wanted = mode === "auto" ? ["claude-code", "codex"] : [mode];
    return (Array.isArray(targets) ? targets : []).filter((target) =>
      Array.isArray(target?.providers) && target.providers.some((provider) => wanted.includes(provider)) &&
      (cloud === undefined || isCloudTarget(target) === Boolean(cloud)));
  }

  function agentPlacement(data, mode, wantCloud) {
    if (!data || !Array.isArray(data.targets)) return null;
    const canCloud = eligibleTargets(data.targets, mode, true).length > 0;
    const canDesktop = eligibleTargets(data.targets, mode, false).length > 0;
    const ready = canCloud || canDesktop;
    return {
      cloud: canCloud && (wantCloud || !canDesktop),
      canCloud,
      canDesktop,
      ready,
      otherAgent: !ready && data.targets.some((target) => Array.isArray(target?.providers) && target.providers.length > 0),
      denied: !ready && [401, 403, 404].includes(Number(data.errorStatus || 0)),
      failed: !ready && Boolean(data.error),
    };
  }

  function placementNotice(placement, repo, mode) {
    if (!placement || placement.ready || !repo) return null;
    if (placement.denied) {
      return { text: `Only ${repo.owner} can start agents on ${repo.display}. Use task to ask for this instead.`, links: [] };
    }
    if (placement.failed) {
      return { text: `Agent targets for ${repo.display} could not be loaded. Use refresh to try again.`, links: [] };
    }
    if (placement.otherAgent) {
      const label = AGENTS.find((agent) => agent.value === mode)?.label || mode;
      return { text: `Nothing that serves ${repo.display} runs ${label}. Pick another model.`, links: [] };
    }
    return {
      text: `To run agents on ${repo.display}, set up a ForkMesh runner on any Linux machine or open the desktop app.`,
      links: [
        { href: RUNNER_SETUP_PAGE, label: "Set up a runner" },
        { href: DESKTOP_PAGE, label: "Get the desktop app" },
      ],
    };
  }

  function ownerAgentEndpoint(repo) {
    return `/api/repo/${encodeURIComponent(repo.owner)}/${encodeURIComponent(repo.name)}/agent-sessions`;
  }

  function withOwnerRoute(primary, owner, endpoint) {
    if (!owner) return primary;
    const nodes = new Set(primary.targets.map((target) => String(target?.node || "")));
    const targets = owner.targets
      .filter((target) => (primary.error || isCloudTarget(target)) && !nodes.has(String(target?.node || "")))
      .map((target) => ({ ...target, endpoint }));
    const ids = new Set(primary.sessions.map((session) => session?.id));
    const sessions = owner.sessions
      .filter((session) => !ids.has(session?.id))
      .map((session) => ({ ...session, endpoint }));
    return {
      targets: [...primary.targets, ...targets],
      sessions: [...primary.sessions, ...sessions],
      models: primary.models || owner.models,
      error: "",
    };
  }

  function pickTarget(targets, mode, cloud) {
    return eligibleTargets(targets, mode, cloud).sort((a, b) =>
      Number(Boolean(b.online)) - Number(Boolean(a.online)) ||
      Number(Boolean(b.source)) - Number(Boolean(a.source)))[0] || null;
  }

  function promptableSessions(sessions) {
    return (Array.isArray(sessions) ? sessions : [])
      .filter((session) => PROMPTABLE_STATUSES.has(String(session?.status || "")) && Number(session?.localAgentId || 0) > 0)
      .sort((a, b) => Number(b.updatedAt || 0) - Number(a.updatedAt || 0));
  }

  function contextUsage(session) {
    const info = session?.agentInfo || {};
    const used = Number(info.contextTokens || 0);
    const windowSize = Number(info.contextWindow || 0);
    if (!(used > 0 && windowSize > 0)) return null;
    const ratio = Math.max(0, Math.min(1, used / windowSize));
    return { used, windowSize, ratio, bars: Math.max(1, Math.round(ratio * GAUGE_BARS)) };
  }

  function formatTokens(value) {
    const number = Number(value || 0);
    if (number >= 1_000_000) return (number / 1_000_000).toFixed(number % 1_000_000 ? 1 : 0) + "M";
    if (number >= 1000) return Math.round(number / 1000) + "k";
    return String(number);
  }

  function formatBytes(value) {
    const number = Number(value || 0);
    if (number >= 1024 * 1024) return (number / (1024 * 1024)).toFixed(1) + " MB";
    if (number >= 1024) return Math.round(number / 1024) + " KB";
    return number + " B";
  }

  function readFileAs(file, how) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => resolve(reader.result);
      reader.onerror = () => reject(reader.error || new Error("read_failed"));
      if (how === "text") reader.readAsText(file);
      else reader.readAsDataURL(file);
    });
  }

  function dataUrlParts(dataUrl) {
    const match = String(dataUrl || "").match(/^data:([^;,]+);base64,(.*)$/);
    return match ? { mediaType: match[1].toLowerCase(), base64: match[2] } : null;
  }

  function base64Size(base64) {
    const text = String(base64 || "");
    const padding = text.endsWith("==") ? 2 : text.endsWith("=") ? 1 : 0;
    return Math.max(0, Math.floor(text.length * 3 / 4) - padding);
  }

  function loadImage(src) {
    return new Promise((resolve, reject) => {
      const image = new Image();
      image.onload = () => resolve(image);
      image.onerror = () => reject(new Error("image_decode_failed"));
      image.src = src;
    });
  }

  async function shrinkImage(dataUrl, maxBytes) {
    const image = await loadImage(dataUrl);
    let scale = Math.min(1, 2560 / Math.max(image.naturalWidth || 1, image.naturalHeight || 1));
    let quality = 0.88;
    for (let attempt = 0; attempt < 9; attempt += 1) {
      const canvas = document.createElement("canvas");
      canvas.width = Math.max(1, Math.round(image.naturalWidth * scale));
      canvas.height = Math.max(1, Math.round(image.naturalHeight * scale));
      const context = canvas.getContext("2d");
      context.fillStyle = "#ffffff";
      context.fillRect(0, 0, canvas.width, canvas.height);
      context.drawImage(image, 0, 0, canvas.width, canvas.height);
      const out = canvas.toDataURL("image/jpeg", quality);
      const parts = dataUrlParts(out);
      if (parts && base64Size(parts.base64) <= maxBytes) return out;
      scale *= 0.8;
      quality = Math.max(0.6, quality - 0.06);
    }
    return null;
  }

  function isTextLike(file) {
    const type = String(file?.type || "").toLowerCase();
    if (type.startsWith("text/")) return true;
    if (/(json|xml|yaml|toml|javascript|typescript|x-sh|x-python|sql|csv|markdown)/.test(type)) return true;
    return /\.(txt|md|markdown|json|ya?ml|toml|ini|cfg|conf|csv|tsv|log|xml|html?|css|scss|js|mjs|cjs|ts|tsx|jsx|py|rb|go|rs|java|kt|swift|c|cc|cpp|h|hpp|cs|php|sh|bash|zsh|sql|diff|patch|env\.example|gitignore|dockerfile)$/i
      .test(String(file?.name || "")) || /^(dockerfile|makefile|readme|license|changelog)$/i.test(String(file?.name || ""));
  }

  function attachmentText(item) {
    return `\n\nAttached file: ${item.name}\n\`\`\`\n${item.text}\n\`\`\``;
  }

  const composer = {
    session: null,
    account: "",
    stored: {},
    root: null,
    els: {},
    attachments: [],
    context: null,
    dismissedContextKey: "",
    openSessionId: "",
    repoCache: new Map(),
    repoChoices: [],
    appCommands: null,
    historyIndex: -1,
    historyScratch: "",
    busy: false,
    recognition: null,
    listening: false,
    menu: { open: false, items: [], active: 0, filter: "", fromTyping: false },
    choice: { open: "", active: -1, from: "" },
    saveTimer: 0,
    drag: null,
    pageHandlers: [],
    inline: false,
  };

  function storageKey() {
    return composer.account ? STORAGE_PREFIX + composer.account : "";
  }

  function loadStored() {
    const key = storageKey();
    if (!key) return {};
    try {
      const value = JSON.parse(localStorage.getItem(key) || "{}");
      return value && typeof value === "object" ? value : {};
    } catch (_) {
      return {};
    }
  }

  function persist() {
    const key = storageKey();
    if (!key) return;
    try {
      localStorage.setItem(key, JSON.stringify(composer.stored));
    } catch (_) {}
  }

  function setStored(patch) {
    Object.assign(composer.stored, patch);
    persist();
  }

  function scheduleDraftSave() {
    window.clearTimeout(composer.saveTimer);
    composer.saveTimer = window.setTimeout(() => {
      setStored({ draft: composer.els.input?.value || "" });
    }, 250);
  }

  function recentRepos() {
    try {
      const values = JSON.parse(localStorage.getItem(RECENT_REPOS_PREFIX + composer.account) || "[]");
      return (Array.isArray(values) ? values : []).map((value) => String(value || "")).filter((value) => value.includes("/"));
    } catch (_) {
      return [];
    }
  }

  function currentAgent() {
    const agent = String(composer.stored.agent || composer.stored.mode || "auto");
    return AGENTS.some((option) => option.value === agent) ? agent : "auto";
  }

  function currentRunMode() {
    const mode = String(composer.stored.runMode || DEFAULT_RUN_MODE);
    return RUN_MODES.some((option) => option.value === mode) ? mode : DEFAULT_RUN_MODE;
  }

  function currentEffort() {
    const effort = String(composer.stored.effort || DEFAULT_EFFORT);
    return EFFORTS.some((option) => option.value === effort) ? effort : DEFAULT_EFFORT;
  }

  function modeProvider(mode) {
    return mode !== "auto" && MODEL_CATALOG[mode] ? mode : "claude-code";
  }

  function currentModel() {
    const provider = modeProvider(currentAgent());
    const options = modelOptions(currentAgent(), activeRepoData()?.models);
    const stored = String((composer.stored.models || {})[provider] || DEFAULT_MODEL[provider]);
    return options.some((option) => option.value === stored)
      ? stored
      : (options.find((option) => option.value === DEFAULT_MODEL[provider]) || options[0])?.value || "";
  }

  function activeRepo() {
    if (composer.context && composer.dismissedContextKey !== composer.context.key) return composer.context.repo;
    const picked = String(composer.stored.repo || "");
    if (!picked) return null;
    return composer.repoChoices.find((repo) => repo.display.toLowerCase() === picked.toLowerCase()) ||
      repoFromDisplay(picked, composer.session);
  }

  function activeContext() {
    return composer.context && composer.dismissedContextKey !== composer.context.key ? composer.context : null;
  }

  function activeRepoData() {
    const repo = activeRepo();
    return repo ? composer.repoCache.get(repo.display.toLowerCase()) || null : null;
  }

  function selectedSession() {
    const open = openSession();
    if (!open) return null;
    const sessions = promptableSessions(activeRepoData()?.sessions);
    const chosen = String(composer.els.sessionSelect?.value || "");
    return sessions.find((session) => session.id === chosen) || open;
  }

  function openSession() {
    const selected = hostSelectedSessionId();
    if (!selected || !activeContext()) return null;
    return promptableSessions(activeRepoData()?.sessions).find((session) => session.id === selected) || null;
  }

  function hostSelectedSessionId() {
    try {
      return String(host()?.selectedAgentId?.() || "");
    } catch (_) {
      return "";
    }
  }

  function pageHandler() {
    for (const handler of composer.pageHandlers) {
      try {
        if (handler.isActive()) return handler;
      } catch (_) {}
    }
    return null;
  }

  function enterAction() {
    if (pageHandler()) return "page";
    return openSession() ? "add" : "new";
  }

  function inlineTextLength() {
    return composer.attachments
      .filter((item) => item.kind === "text")
      .reduce((sum, item) => sum + attachmentText(item).length, 0);
  }

  function remainingChars() {
    const text = composer.els.input?.value || "";
    return MAX_PROMPT - text.length - contextSuffix(activeContext()).length - inlineTextLength();
  }

  function buildMarkup() {
    const root = document.createElement("div");
    root.className = "fmc-root";
    root.dataset.composer = "";
    root.innerHTML = `
      <button type="button" class="fmc-pill" data-composer-pill aria-expanded="false" aria-controls="fmcPanel" title="Open prompt composer (${escapeHtml(SHORTCUT_LABEL)})">
        ${icon("prompt")}<span class="fmc-pill-label">Prompt</span><span class="fmc-pill-dot" data-composer-draft-dot hidden></span>
      </button>
      <section id="fmcPanel" class="fmc-panel" data-composer-panel role="region" aria-label="Prompt composer" hidden>
        <div class="fmc-title">
          <button type="button" class="fmc-icon-button" data-composer-clear aria-label="Clear prompt" title="Clear prompt">${icon("x")}</button>
          <button type="button" class="fmc-dot" data-composer-pr aria-pressed="true" title="Open a pull request when the run finishes"><span class="fmc-dot-mark"></span><span class="fmc-sr">Open a pull request when the run finishes</span></button>
          <button type="button" class="fmc-icon-button" data-composer-cloud aria-pressed="false" aria-label="Run new agents in the cloud" title="Run new agents in the cloud">${icon("cloud")}</button>
          <div class="fmc-handle" data-composer-handle title="Drag to move; double-click to reset" aria-hidden="true">${icon("grip")}</div>
          <span class="fmc-count" data-composer-count aria-label="Characters left" hidden>${MAX_PROMPT}</span>
          <button type="button" class="fmc-icon-button" data-composer-reset aria-label="Reset position and refresh" title="Reset position and refresh">${icon("refresh")}</button>
          <button type="button" class="fmc-icon-button" data-composer-expand aria-pressed="false" aria-label="Expand composer" title="Expand composer">${icon("expand")}</button>
          <button type="button" class="fmc-icon-button" data-composer-collapse aria-label="Collapse composer" title="Collapse composer (Esc)">${icon("minimize")}</button>
        </div>
        <div class="fmc-box">
          <div class="fmc-meta" data-composer-meta>
            <span class="fmc-chip" data-composer-chip hidden>
              <span class="fmc-chip-icon" data-composer-chip-icon></span>
              <span class="fmc-chip-label" data-composer-chip-label></span>
              <button type="button" class="fmc-chip-remove" data-composer-chip-remove aria-label="Remove context">${icon("x")}</button>
            </span>
            <label class="fmc-field" data-composer-session-field hidden>
              <span class="fmc-field-label">add to</span>
              <select class="fmc-select" data-composer-session aria-label="Session to add the prompt to"></select>
              ${icon("chevron", "fmc-field-chevron")}
            </label>
            <label class="fmc-field" data-composer-target-field hidden>
              ${icon("server", "fmc-field-icon")}
              <span class="fmc-sr">Node that runs a new agent</span>
              <select class="fmc-select" data-composer-target aria-label="Node that runs a new agent"></select>
              ${icon("chevron", "fmc-field-chevron")}
            </label>
          </div>
          <ul class="fmc-attachments" data-composer-attachments aria-label="Attachments" hidden></ul>
          <div class="fmc-body">
            <label class="fmc-sr" for="fmcInput">Prompt</label>
            <textarea id="fmcInput" class="fmc-input" data-composer-input rows="3" maxlength="${MAX_PROMPT}" placeholder="${DEFAULT_PLACEHOLDER}" spellcheck="true" aria-describedby="fmcStatus" autocomplete="off"></textarea>
          </div>
          <div class="fmc-menu" data-composer-menu role="listbox" id="fmcMenu" aria-label="Slash commands" tabindex="-1" hidden></div>
          <div class="fmc-toolbar">
            <div class="fmc-tools">
              <button type="button" class="fmc-avatar" data-composer-avatar aria-label="Collapse composer" title="Collapse composer" hidden></button>
              <button type="button" class="fmc-icon-button" data-composer-attach aria-label="Attach photos or files" title="Attach photos or files">${icon("clip")}</button>
              <button type="button" class="fmc-icon-button" data-composer-mic aria-pressed="false" aria-label="Dictate" title="Dictate">${icon("mic")}</button>
              <button type="button" class="fmc-icon-button fmc-slash" data-composer-slash aria-haspopup="listbox" aria-expanded="false" aria-controls="fmcMenu" aria-label="Slash commands" title="Slash commands">/</button>
              <label class="fmc-field" data-composer-repo-field hidden>
                ${icon("repo", "fmc-field-icon")}
                <span class="fmc-sr">Repository</span>
                <select class="fmc-select" data-composer-repo></select>
                ${icon("chevron", "fmc-field-chevron")}
              </label>
              ${choiceMarkup("mode", "Permission mode")}
            </div>
            <div class="fmc-tools fmc-tools-end">
              <span class="fmc-gauge" data-composer-gauge role="img" aria-label="No context usage reported" title="No context usage reported">${"<span></span>".repeat(GAUGE_BARS)}</span>
              <div class="fmc-choice-pair">
                ${choiceMarkup("model", "Model")}
                ${choiceMarkup("speed", "Reasoning effort")}
              </div>
              <button type="button" class="fmc-action" data-composer-task title="Create an organization task from this prompt">Task</button>
              <button type="button" class="fmc-action" data-composer-add title="Add this prompt to the selected agent session">Add</button>
              <button type="button" class="fmc-action fmc-action-primary" data-composer-page hidden><span data-composer-page-label></span>${icon("add")}</button>
              <button type="button" class="fmc-action fmc-action-primary fmc-send" data-composer-new title="Start a new agent session with this prompt">${icon("add")}<span class="fmc-sr">Start a new agent</span></button>
            </div>
            <input type="file" data-composer-file multiple hidden tabindex="-1" />
          </div>
        </div>
        <div class="fmc-output" data-composer-output hidden></div>
        <p class="fmc-setup" data-composer-setup hidden></p>
        <p id="fmcStatus" class="fmc-status" data-composer-status role="status" aria-live="polite"></p>
      </section>`;
    return root;
  }

  function choiceMarkup(key, label) {
    return `<div class="fmc-choice" data-composer-choice="${key}">
            <button type="button" class="fmc-choice-button" data-composer-${key} aria-haspopup="listbox" aria-expanded="false" aria-controls="fmcChoice-${key}" aria-label="${label}"></button>
            <div class="fmc-choice-menu" id="fmcChoice-${key}" data-composer-choice-menu="${key}" role="listbox" aria-label="${label}" tabindex="-1" hidden></div>
          </div>`;
  }

  function $(selector) {
    return composer.root?.querySelector(selector) || null;
  }

  function bindElements() {
    composer.els = {
      pill: $("[data-composer-pill]"),
      draftDot: $("[data-composer-draft-dot]"),
      panel: $("[data-composer-panel]"),
      clear: $("[data-composer-clear]"),
      pr: $("[data-composer-pr]"),
      cloud: $("[data-composer-cloud]"),
      handle: $("[data-composer-handle]"),
      count: $("[data-composer-count]"),
      reset: $("[data-composer-reset]"),
      expand: $("[data-composer-expand]"),
      collapse: $("[data-composer-collapse]"),
      chip: $("[data-composer-chip]"),
      chipIcon: $("[data-composer-chip-icon]"),
      chipLabel: $("[data-composer-chip-label]"),
      chipRemove: $("[data-composer-chip-remove]"),
      repoField: $("[data-composer-repo-field]"),
      repoSelect: $("[data-composer-repo]"),
      sessionField: $("[data-composer-session-field]"),
      sessionSelect: $("[data-composer-session]"),
      targetField: $("[data-composer-target-field]"),
      targetSelect: $("[data-composer-target]"),
      input: $("[data-composer-input]"),
      task: $("[data-composer-task]"),
      add: $("[data-composer-add]"),
      create: $("[data-composer-new]"),
      page: $("[data-composer-page]"),
      pageLabel: $("[data-composer-page-label]"),
      attachments: $("[data-composer-attachments]"),
      output: $("[data-composer-output]"),
      menu: $("[data-composer-menu]"),
      avatar: $("[data-composer-avatar]"),
      attach: $("[data-composer-attach]"),
      mic: $("[data-composer-mic]"),
      slash: $("[data-composer-slash]"),
      model: $("[data-composer-model]"),
      mode: $("[data-composer-mode]"),
      speed: $("[data-composer-speed]"),
      gauge: $("[data-composer-gauge]"),
      file: $("[data-composer-file]"),
      status: $("[data-composer-status]"),
      setup: $("[data-composer-setup]"),
    };
  }

  function setStatus(text, tone = "") {
    const status = composer.els.status;
    if (!status) return;
    status.textContent = text || "";
    status.dataset.tone = tone;
  }

  function setOutput(nodeOrText, link) {
    const output = composer.els.output;
    if (!output) return;
    output.replaceChildren();
    if (!nodeOrText) {
      output.hidden = true;
      return;
    }
    if (typeof nodeOrText === "string") {
      const paragraph = document.createElement("p");
      paragraph.textContent = nodeOrText;
      output.append(paragraph);
    } else {
      output.append(nodeOrText);
    }
    if (link?.href) {
      const anchor = document.createElement("a");
      anchor.href = link.href;
      anchor.textContent = link.label || link.href;
      output.append(anchor);
    }
    output.hidden = false;
  }

  function isOpen() {
    return Boolean(composer.els.panel && !composer.els.panel.hidden);
  }

  function setOpen(open, { focus = true } = {}) {
    const { panel, pill } = composer.els;
    if (!panel || !pill) return;
    if (composer.inline && !open) {
      closeMenu();
      closeChoice({ focus: false });
      stopDictation();
      return;
    }
    const wasOpen = !panel.hidden;
    panel.hidden = !open;
    pill.hidden = open;
    pill.setAttribute("aria-expanded", String(open));
    composer.root.dataset.open = open ? "1" : "0";
    if (!composer.inline && composer.stored.open !== open) setStored({ open });
    if (open) {
      refreshContext();
      applyPlacement();
      void ensureRepoChoices();
      void loadRepoData();
      fitInput();
      if (focus) focusInput(!wasOpen);
    } else {
      closeMenu();
      closeChoice({ focus: false });
      stopDictation();
      if (focus) pill.focus();
    }
  }

  function isExpanded() {
    return composer.stored.large === true;
  }

  function focusInput(toEnd) {
    const input = composer.els.input;
    if (!input) return;
    input.focus({ preventScroll: true });
    if (!toEnd) return;
    const end = input.value.length;
    input.setSelectionRange(end, end);
  }

  function applyPlacement() {
    const panel = composer.els.panel;
    if (!panel) return;
    const expanded = isExpanded();
    panel.classList.toggle("fmc-expanded", expanded);
    fitInput();
    composer.els.expand?.setAttribute("aria-pressed", String(expanded));
    if (composer.els.expand) {
      composer.els.expand.innerHTML = icon(expanded ? "shrink" : "expand");
      composer.els.expand.setAttribute("aria-label", expanded ? "Shrink composer" : "Expand composer");
      composer.els.expand.title = expanded ? "Shrink composer" : "Expand composer";
    }
    const position = composer.stored.position;
    if (position && !composer.inline && window.matchMedia("(min-width: 640px)").matches) {
      const right = Math.max(8, Math.min(Number(position.right) || 16, window.innerWidth - 120));
      const bottom = Math.max(8, Math.min(Number(position.bottom) || 16, window.innerHeight - 80));
      panel.style.right = right + "px";
      panel.style.bottom = bottom + "px";
    } else {
      panel.style.right = "";
      panel.style.bottom = "";
    }
  }

  function renderAvatar() {
    const avatar = composer.els.avatar;
    if (!avatar) return;
    const name = String(composer.session?.nodeName || composer.session?.email || "?");
    const headerImage = document.querySelector("[data-dashboard-profile-avatar-image]");
    const src = headerImage && !headerImage.classList.contains("hidden") ? String(headerImage.getAttribute("src") || "") : "";
    avatar.replaceChildren();
    if (/^(https?:|data:image\/|\/)/.test(src)) {
      const image = document.createElement("img");
      image.src = src;
      image.alt = "";
      avatar.append(image);
    } else {
      const initial = document.createElement("span");
      initial.textContent = name.charAt(0).toUpperCase();
      avatar.append(initial);
    }
  }

  function renderPr() {
    const on = composer.stored.createPr === true;
    composer.els.pr?.setAttribute("aria-pressed", String(on));
    if (composer.els.pr) {
      const label = on
        ? "This agent opens a pull request when it finishes (on)"
        : "This agent opens a pull request when it finishes (off)";
      composer.els.pr.title = label;
      composer.els.pr.querySelector(".fmc-sr").textContent = label;
    }
  }

  function currentPlacement() {
    return agentPlacement(activeRepoData(), currentAgent(), composer.stored.cloud === true);
  }

  function runsInCloud() {
    const placement = currentPlacement();
    return placement ? placement.cloud : composer.stored.cloud === true;
  }

  function canToggleCloud() {
    const placement = currentPlacement();
    return !placement || (placement.canCloud && placement.canDesktop);
  }

  function canStartNew() {
    return currentPlacement()?.ready !== false;
  }

  function renderCloud() {
    const button = composer.els.cloud;
    if (!button) return;
    const placement = currentPlacement();
    const on = runsInCloud();
    const where = activeRepo()?.display || "this repository";
    const label = !placement || (placement.canCloud && placement.canDesktop)
      ? (on
        ? "New agents run on a ForkMesh cloud runner (on); click to run them on your desktop"
        : "New agents run on your desktop (cloud off); click to run them on a ForkMesh cloud runner")
      : placement.canCloud
        ? `New agents run on a ForkMesh cloud runner; no desktop serves ${where}`
        : placement.canDesktop
          ? `New agents run on your desktop; no ForkMesh cloud runner serves ${where}`
          : `No desktop or cloud runner serves ${where} yet`;
    button.disabled = !canToggleCloud();
    button.setAttribute("aria-pressed", String(on));
    button.setAttribute("aria-label", label);
    button.title = label;
  }

  function renderAvailability() {
    renderCloud();
    const notice = placementNotice(currentPlacement(), activeRepo(), currentAgent());
    const { create, setup } = composer.els;
    if (create) create.disabled = composer.busy || !canStartNew();
    if (!setup) return;
    setup.replaceChildren();
    setup.hidden = !notice;
    if (!notice) return;
    setup.append(document.createTextNode(notice.text));
    for (const link of notice.links) {
      const anchor = document.createElement("a");
      anchor.href = link.href;
      anchor.textContent = link.label;
      setup.append(" ", anchor);
    }
  }

  function renderCount() {
    const left = remainingChars();
    const count = composer.els.count;
    if (count) {
      count.hidden = left > MAX_PROMPT * 0.2;
      count.textContent = String(left);
      count.dataset.tone = left < 0 ? "over" : left <= MAX_PROMPT * 0.05 ? "near" : left <= MAX_PROMPT * 0.2 ? "low" : "";
      count.setAttribute("aria-label", left < 0 ? `${-left} characters over the limit` : `${left} characters left`);
    }
    const hasDraft = Boolean((composer.els.input?.value || "").trim() || composer.attachments.length);
    if (composer.els.draftDot) composer.els.draftDot.hidden = !hasDraft;
    fitInput();
  }

  function fitInput() {
    const input = composer.els.input;
    if (!input || !isOpen()) return;
    input.style.height = "auto";
    input.style.height = input.scrollHeight + "px";
  }

  function choiceItems(key) {
    if (key === "mode") {
      return RUN_MODES.map((option) => ({ ...option, title: `${option.label} — ${option.hint}` }));
    }
    if (key === "speed") {
      return EFFORTS.map((option) => ({ ...option, title: `Reasoning effort: ${option.label}` }));
    }
    const allowed = activeRepoData()?.models;
    const effort = EFFORTS.find((option) => option.value === currentEffort());
    return [
      ...AGENTS.filter((agent) => MODEL_CATALOG[agent.value]).flatMap((agent) =>
        modelOptions(agent.value, allowed).map((option) => ({
          value: `${agent.value}|${option.value}`,
          label: option.label,
          hint: option.id,
          group: agent.label,
          title: `Model: ${option.label} (${option.id}) on ${agent.label}`,
        }))),
      { value: EFFORT_ROW, label: "Effort", hint: effort?.label || "", submenu: true, separator: true, title: "Reasoning effort" },
    ];
  }

  function choiceValue(key) {
    if (key === "mode") return currentRunMode();
    if (key === "speed") return currentEffort();
    return `${modeProvider(currentAgent())}|${currentModel()}`;
  }

  function choiceSummary(key) {
    const item = choiceItems(key).find((entry) => entry.value === choiceValue(key));
    if (key === "mode") return `Permission mode: ${item?.label || ""}. How much freedom the agent has to make changes without asking first.`;
    if (key === "speed") return `Reasoning effort: ${item?.label || ""}. Higher levels are slower and more thorough.`;
    const agent = currentAgent() === "auto" ? " · the node runs the first agent it has installed" : "";
    return item ? `${item.title}${agent}` : "Model";
  }

  function renderChoice(key) {
    const button = composer.els[key];
    if (!button) return;
    const item = choiceItems(key).find((entry) => entry.value === choiceValue(key));
    const chevron = key === "model" ? "" : icon("chevron", "fmc-choice-chevron");
    button.innerHTML = `${choiceGlyph(key, item?.value)}<span class="fmc-choice-label">${escapeHtml(item?.label || "")}</span>${chevron}`;
    button.dataset.value = item ? item.value.split("|").pop() : "";
    const summary = choiceSummary(key);
    button.title = summary;
    button.setAttribute("aria-label", summary);
    if (composer.choice.open === key) renderChoiceMenu(key);
  }

  function renderModes() {
    renderChoice("mode");
    renderChoice("speed");
  }

  function renderModels() {
    renderChoice("model");
  }

  function renderChoiceMenu(key) {
    const menu = $(`[data-composer-choice-menu="${key}"]`);
    if (!menu) return;
    const items = choiceItems(key);
    const selected = choiceValue(key);
    if (composer.choice.active < 0 || composer.choice.active >= items.length) {
      composer.choice.active = Math.max(0, items.findIndex((item) => item.value === selected));
    }
    let group = "";
    menu.innerHTML = items.map((item, index) => {
      const separator = item.separator ? `<div class="fmc-menu-sep" role="presentation"></div>` : "";
      const header = item.group && item.group !== group ? `<div class="fmc-menu-group" role="presentation">${escapeHtml(item.group)}</div>` : "";
      group = item.group || group;
      const hint = item.hint ? `<span class="fmc-menu-hint">${escapeHtml(item.hint)}</span>` : "";
      const mark = item.submenu ? icon("chevronRight", "fmc-choice-mark") : icon("check", "fmc-choice-mark fmc-choice-check");
      return `${separator}${header}<div class="fmc-choice-option" role="option" id="fmcChoice-${key}-${index}" data-composer-choice-option="${index}" aria-selected="${item.value === selected}" data-active="${index === composer.choice.active ? "1" : "0"}"${item.submenu ? ' data-submenu="1"' : ""} title="${escapeHtml(item.title || item.label)}">${choiceGlyph(key, item.value)}<span class="fmc-choice-option-label">${escapeHtml(item.label)}</span>${hint}${mark}</div>`;
    }).join("");
    menu.setAttribute("aria-activedescendant", `fmcChoice-${key}-${composer.choice.active}`);
    menu.querySelector('[data-active="1"]')?.scrollIntoView?.({ block: "nearest" });
  }

  function openChoice(key, from = "") {
    if (composer.choice.open && composer.choice.open !== key) closeChoice({ focus: false });
    closeMenu();
    const menu = $(`[data-composer-choice-menu="${key}"]`);
    const button = composer.els[key];
    if (!menu || !button) return;
    composer.choice = { open: key, active: -1, from };
    renderChoiceMenu(key);
    menu.dataset.drop = "";
    menu.style.translate = "";
    menu.hidden = false;
    const bounds = composer.els.panel?.getBoundingClientRect() || { left: 0, right: window.innerWidth };
    const box = menu.getBoundingClientRect();
    const shift = Math.max(bounds.left + 4 - box.left, Math.min(0, bounds.right - 4 - box.right));
    if (shift) menu.style.translate = `${shift}px 0`;
    const rect = button.getBoundingClientRect();
    const viewport = window.visualViewport?.height || window.innerHeight;
    if (menu.offsetHeight > rect.top - 8 && viewport - rect.bottom > rect.top) menu.dataset.drop = "down";
    button.setAttribute("aria-expanded", "true");
    menu.focus();
  }

  function closeChoice({ focus = true } = {}) {
    const key = composer.choice.open;
    if (!key) return;
    composer.choice = { open: "", active: -1, from: "" };
    const menu = $(`[data-composer-choice-menu="${key}"]`);
    if (menu) menu.hidden = true;
    composer.els[key]?.setAttribute("aria-expanded", "false");
    if (focus) composer.els[key]?.focus();
  }

  function pickChoice(key, value) {
    if (key === "mode") setRunMode(value);
    else if (key === "speed") setEffort(value);
    else if (value === EFFORT_ROW) openChoice("speed", "model");
    else {
      const [agent, model] = String(value).split("|");
      setAgentModel(agent, model);
    }
  }

  function onChoiceKeydown(event, key) {
    const items = choiceItems(key);
    const last = items.length - 1;
    const moves = { ArrowDown: 1, ArrowUp: -1 };
    if (moves[event.key]) {
      composer.choice.active = Math.min(last, Math.max(0, composer.choice.active + moves[event.key]));
    } else if (event.key === "Home") {
      composer.choice.active = 0;
    } else if (event.key === "End") {
      composer.choice.active = last;
    } else if (event.key === "Enter" || event.key === " " || (event.key === "ArrowRight" && items[composer.choice.active]?.submenu)) {
      const item = items[composer.choice.active];
      closeChoice();
      if (item) pickChoice(key, item.value);
    } else if (event.key === "ArrowLeft" && composer.choice.from) {
      const back = composer.choice.from;
      closeChoice({ focus: false });
      openChoice(back);
      composer.choice.active = choiceItems(back).findIndex((item) => item.value === EFFORT_ROW);
      renderChoiceMenu(back);
      event.preventDefault();
      event.stopPropagation();
      return;
    } else if (event.key === "Escape") {
      closeChoice();
    } else if (event.key === "Tab") {
      closeChoice({ focus: false });
      return;
    } else {
      return;
    }
    event.preventDefault();
    event.stopPropagation();
    if (composer.choice.open === key) renderChoiceMenu(key);
  }

  function renderContext() {
    const { chip, chipIcon, chipLabel, repoField, repoSelect } = composer.els;
    const context = activeContext();
    if (chip) chip.hidden = !context;
    if (context) {
      chipIcon.innerHTML = icon(context.kind === "issue" ? "issue" : context.kind === "pull" ? "pull" : "repo");
      chipLabel.textContent = context.label;
      chip.title = `Context: ${context.label}`;
      composer.els.chipRemove.setAttribute("aria-label", `Remove context ${context.label}`);
    }
    if (repoField) repoField.hidden = Boolean(context);
    if (!context && repoSelect) {
      const picked = String(composer.stored.repo || "").toLowerCase();
      const seen = new Set();
      const choices = [];
      for (const repo of composer.repoChoices) {
        const key = repo.display.toLowerCase();
        if (!seen.has(key)) {
          seen.add(key);
          choices.push(repo.display);
        }
      }
      for (const display of recentRepos()) {
        const key = display.toLowerCase();
        if (!seen.has(key)) {
          seen.add(key);
          choices.push(display);
        }
      }
      if (picked && !seen.has(picked)) choices.unshift(composer.stored.repo);
      repoSelect.innerHTML = `<option value="">Choose a repository…</option>` + choices.map((display) =>
        `<option value="${escapeHtml(display)}"${display.toLowerCase() === picked ? " selected" : ""}>${escapeHtml(display)}</option>`).join("");
    }
  }

  function renderSessionsAndTargets() {
    const data = activeRepoData();
    const { sessionField, sessionSelect, targetField, targetSelect } = composer.els;
    const sessions = promptableSessions(data?.sessions);
    if (sessionField && sessionSelect) {
      const previous = String(sessionSelect.value || "");
      const preferred = hostSelectedSessionId();
      const selected = sessions.find((session) => session.id === previous) ||
        sessions.find((session) => session.id === preferred) || sessions[0];
      sessionSelect.innerHTML = sessions.map((session) => {
        const title = String(session.title || "").trim() || `Session ${String(session.id).slice(0, 8)}`;
        return `<option value="${escapeHtml(session.id)}"${session === selected ? " selected" : ""}>${escapeHtml(title.slice(0, 60))} · ${escapeHtml(session.status)}</option>`;
      }).join("");
    }
    if (targetField && targetSelect) {
      const mode = currentAgent();
      const cloud = runsInCloud();
      const targets = eligibleTargets(data?.targets, mode, cloud);
      const previous = String(targetSelect.value || "");
      const best = pickTarget(data?.targets, mode, cloud);
      const kept = targets.some((target) => target.node === previous);
      targetSelect.innerHTML = targets.map((target) =>
        `<option value="${escapeHtml(target.node || "")}"${(kept ? target.node === previous : target === best) ? " selected" : ""}>${escapeHtml(target.node || "node")}${target.online ? "" : " (offline)"}</option>`).join("");
      targetField.hidden = targets.length < 2;
    }
    renderAddVisibility();
    renderAvailability();
  }

  function renderAddVisibility() {
    const open = openSession();
    const { sessionField, sessionSelect, add } = composer.els;
    if (open && sessionSelect && open.id !== composer.openSessionId) sessionSelect.value = open.id;
    composer.openSessionId = open?.id || "";
    if (sessionField) sessionField.hidden = !open;
    if (add) add.hidden = !open;
    renderGauge();
  }

  function renderGauge() {
    const gauge = composer.els.gauge;
    if (!gauge) return;
    const usage = contextUsage(selectedSession());
    const bars = gauge.querySelectorAll("span");
    bars.forEach((bar, index) => {
      bar.dataset.on = usage && index < usage.bars ? "1" : "0";
    });
    gauge.dataset.level = usage ? (usage.ratio >= 0.9 ? "high" : usage.ratio >= 0.7 ? "mid" : "low") : "none";
    const label = usage
      ? `Context: ${formatTokens(usage.used)} of ${formatTokens(usage.windowSize)} tokens (${Math.round(usage.ratio * 100)}%) in the selected session`
      : "No context usage reported for this repository's sessions";
    gauge.setAttribute("aria-label", label);
    gauge.title = label;
  }

  function renderPage() {
    const handler = pageHandler();
    const { page, pageLabel, create, input } = composer.els;
    if (!page) return;
    page.hidden = !handler;
    pageLabel.textContent = handler?.label || "";
    create?.classList.toggle("fmc-action-primary", !handler);
    if (input) input.placeholder = handler?.placeholder || DEFAULT_PLACEHOLDER;
  }

  function renderEnterTarget() {
    renderPage();
    renderAddVisibility();
    const action = enterAction();
    const handler = pageHandler();
    for (const [name, button] of [["page", composer.els.page], ["add", composer.els.add], ["new", composer.els.create]]) {
      if (!button) continue;
      const base = name === "page" ? (handler?.title || "Send this prompt to the page")
        : name === "add" ? "Add this prompt to the selected agent session" : "Start a new agent session with this prompt";
      button.dataset.enter = action === name ? "1" : "0";
      button.title = action === name ? `${base} (Enter)` : base;
    }
  }

  function renderAttachments() {
    const list = composer.els.attachments;
    if (!list) return;
    list.hidden = !composer.attachments.length;
    list.replaceChildren(...composer.attachments.map((item) => {
      const row = document.createElement("li");
      row.className = "fmc-attachment";
      row.dataset.composerAttachment = item.id;
      if (item.kind === "image") {
        const image = document.createElement("img");
        image.src = item.dataUrl;
        image.alt = "";
        row.append(image);
      } else {
        const glyph = document.createElement("span");
        glyph.className = "fmc-attachment-file";
        glyph.innerHTML = icon("file");
        row.append(glyph);
      }
      const name = document.createElement("span");
      name.className = "fmc-attachment-name";
      name.textContent = `${item.name} · ${formatBytes(item.size)}`;
      row.append(name);
      const remove = document.createElement("button");
      remove.type = "button";
      remove.className = "fmc-attachment-remove";
      remove.dataset.composerAttachmentRemove = item.id;
      remove.setAttribute("aria-label", `Remove ${item.name}`);
      remove.innerHTML = icon("x");
      row.append(remove);
      return row;
    }));
    renderCount();
  }

  function renderAll() {
    renderAvatar();
    renderPr();
    renderCloud();
    renderModes();
    renderModels();
    renderContext();
    renderSessionsAndTargets();
    renderAttachments();
    renderCount();
    renderEnterTarget();
    applyPlacement();
  }

  function refreshContext() {
    const next = detectContext(composer.session);
    const changed = (next?.key || "") !== (composer.context?.key || "");
    composer.context = next;
    renderEnterTarget();
    if (changed) {
      renderContext();
      renderModels();
      renderSessionsAndTargets();
      renderCount();
      renderEnterTarget();
      if (isOpen()) void loadRepoData();
    }
  }

  async function ensureRepoChoices() {
    const bridge = host();
    if (!bridge) return;
    try {
      composer.repoChoices = bridge.repoChoices?.() || [];
      renderContext();
      const loaded = await bridge.loadRepoChoices?.();
      if (Array.isArray(loaded)) {
        composer.repoChoices = loaded;
        renderContext();
      }
    } catch (_) {}
  }

  async function loadRepoData({ force = false } = {}) {
    const repo = activeRepo();
    if (!repo) {
      renderSessionsAndTargets();
      renderModels();
      return null;
    }
    const cacheKey = repo.display.toLowerCase();
    const cached = composer.repoCache.get(cacheKey);
    if (cached && !force && (cached.pending || Date.now() - cached.loadedAt < 60_000)) return cached.pending || cached;
    const load = (path) => api("GET", path).then((payload) => ({
      targets: Array.isArray(payload?.targets) ? payload.targets : [],
      sessions: Array.isArray(payload?.sessions) ? payload.sessions : [],
      models: payload?.models && typeof payload.models === "object" ? payload.models : null,
      error: "",
      errorStatus: 0,
    }));
    const ownerPath = repo.owns ? "" : ownerAgentEndpoint(repo);
    const pending = Promise.all([
      load(repo.endpoint).catch((error) => ({ targets: [], sessions: [], models: null, error: String(error?.message || "Request failed."), errorStatus: Number(error?.status || 0) })),
      ownerPath ? load(ownerPath).catch(() => null) : null,
    ]).then(([primary, owner]) => {
      const data = { loadedAt: Date.now(), ...withOwnerRoute(primary, owner, ownerPath) };
      composer.repoCache.set(cacheKey, data);
      return data;
    });
    composer.repoCache.set(cacheKey, { ...(cached || {}), pending, loadedAt: cached?.loadedAt || 0 });
    const data = await pending;
    if (activeRepo()?.display.toLowerCase() === cacheKey) {
      renderModels();
      renderSessionsAndTargets();
      renderEnterTarget();
    }
    return data;
  }

  function composedPrompt() {
    const text = String(composer.els.input?.value || "").trim();
    const files = composer.attachments.filter((item) => item.kind === "text").map(attachmentText).join("");
    return (text + files + contextSuffix(activeContext())).trim();
  }

  function imageAttachments() {
    return composer.attachments.filter((item) => item.kind === "image");
  }

  function setBusy(busy) {
    composer.busy = busy;
    for (const button of [composer.els.task, composer.els.add, composer.els.create, composer.els.page]) {
      if (button) button.disabled = busy;
    }
    if (composer.els.create && !busy) composer.els.create.disabled = !canStartNew();
    composer.els.panel?.setAttribute("aria-busy", String(busy));
  }

  function rememberHistory(text) {
    const value = String(text || "").trim();
    if (!value) return;
    const history = Array.isArray(composer.stored.history) ? composer.stored.history.filter((entry) => entry !== value) : [];
    history.push(value);
    setStored({ history: history.slice(-HISTORY_LIMIT) });
    composer.historyIndex = -1;
  }

  function clearAfterSend() {
    rememberHistory(composer.els.input.value);
    composer.els.input.value = "";
    composer.attachments = [];
    setStored({ draft: "" });
    renderAttachments();
    renderCount();
  }

  function validateBeforeSend(kind) {
    const text = String(composer.els.input?.value || "").trim();
    if (!text && !(kind === "add" && composer.attachments.length)) {
      setStatus(kind === "task" ? "Type a task first." : "Type a prompt first.", "bad");
      composer.els.input?.focus();
      return false;
    }
    if (remainingChars() < 0) {
      setStatus(`The prompt is ${-remainingChars()} characters over the ${MAX_PROMPT} limit.`, "bad");
      return false;
    }
    return true;
  }

  function agentAttachmentPayload() {
    return imageAttachments().map((item) => ({ name: item.name, mediaType: item.mediaType, data: item.base64 }));
  }

  async function startNewSession() {
    if (composer.busy || !validateBeforeSend("new")) return;
    if (await maybeRunSlashCommand()) return;
    const repo = activeRepo();
    if (!repo) {
      setStatus("Choose a repository for the new agent.", "bad");
      composer.els.repoSelect?.focus();
      return;
    }
    setBusy(true);
    setStatus(`Starting an agent on ${repo.display}…`);
    try {
      const data = await loadRepoData();
      const mode = currentAgent();
      const cloud = runsInCloud();
      const placed = eligibleTargets(data?.targets, mode, cloud);
      if (!placed.length) {
        setStatus("");
        renderAvailability();
        return;
      }
      const chosen = String(composer.els.targetSelect?.value || "");
      const target = placed.find((item) => item.node === chosen) || pickTarget(data?.targets, mode, cloud);
      const targetNode = target?.node || "";
      const body = {
        prompt: composedPrompt(),
        provider: mode,
        model: currentModel(),
        mode: currentRunMode(),
        effort: currentEffort(),
        createPr: composer.stored.createPr === true,
      };
      if (targetNode) body.targetNode = targetNode;
      const attachments = agentAttachmentPayload();
      if (attachments.length) body.attachments = attachments;
      const payload = await api("POST", target?.endpoint || repo.endpoint, body, true);
      const where = payload?.targetNode || targetNode || "the node";
      const message = String(payload?.message || "").trim() ||
        (payload?.queueState === "waiting_for_desktop"
          ? `Saved for ${where}. It will start after the linked desktop reconnects.`
          : `Saved for ${where}. It will start when the node claims it.`);
      clearAfterSend();
      setStored({ createPr: false });
      renderPr();
      setStatus(message, "good");
      setOutput(message, { href: `/${repo.display}/agents`, label: "Open agent sessions" });
      try {
        host()?.rememberRepo?.(repo.display);
        host()?.agentsChanged?.(repo.display, message);
      } catch (_) {}
      void loadRepoData({ force: true });
    } catch (error) {
      setStatus(String(error?.message || "The agent could not be started."), "bad");
    } finally {
      setBusy(false);
    }
  }

  async function addToSession() {
    if (composer.busy || !validateBeforeSend("add")) return;
    if (await maybeRunSlashCommand()) return;
    const repo = activeRepo();
    if (!repo) {
      setStatus("Choose a repository whose agent session should get this prompt.", "bad");
      composer.els.repoSelect?.focus();
      return;
    }
    setBusy(true);
    try {
      await loadRepoData();
      const session = selectedSession();
      if (!session) {
        setStatus(`No running or finished agent session on ${repo.display} can take a follow-up. Use new to start one.`, "bad");
        return;
      }
      setStatus("Adding to the agent session…");
      const body = { prompt: composedPrompt() || "continue" };
      const attachments = agentAttachmentPayload();
      if (attachments.length) body.attachments = attachments;
      const payload = await api("POST", `${session.endpoint || repo.endpoint}/${encodeURIComponent(session.id)}`, body, true);
      const message = String(payload?.message || "Follow-up sent.");
      clearAfterSend();
      setStatus(message, "good");
      setOutput(message, { href: `/${repo.display}/agents`, label: "Open agent sessions" });
      void loadRepoData({ force: true });
    } catch (error) {
      setStatus(String(error?.message || "The follow-up could not be sent."), "bad");
    } finally {
      setBusy(false);
    }
  }

  async function runPageAction() {
    const handler = pageHandler();
    if (!handler) return startNewSession();
    if (composer.busy) return;
    const text = String(composer.els.input?.value || "").trim();
    const images = imageAttachments().map((item) => ({ name: item.name, mediaType: item.mediaType, dataUrl: item.dataUrl }));
    if (!text && !images.length) {
      setStatus(handler.emptyMessage || "Type something first.", "bad");
      composer.els.input?.focus();
      return;
    }
    if (await maybeRunSlashCommand()) return;
    setBusy(true);
    setOutput(null);
    setStatus(handler.busyMessage || "Working…");
    try {
      const result = await handler.submit({ text, images });
      if (!result?.keepPrompt) clearAfterSend();
      setStatus(result?.status || "Done.", result?.tone || "good");
      setOutput(result?.message || null, result?.link);
    } catch (error) {
      setStatus(String(error?.message || error || "That did not work."), "bad");
    } finally {
      setBusy(false);
      renderEnterTarget();
    }
  }

  function registerPageHandler(handler) {
    if (!handler || typeof handler.submit !== "function" || typeof handler.isActive !== "function") {
      throw new Error("invalid_composer_page_handler");
    }
    const entry = Object.freeze({
      id: String(handler.id || ""),
      label: String(handler.label || "send").slice(0, 24),
      title: String(handler.title || ""),
      placeholder: String(handler.placeholder || ""),
      busyMessage: String(handler.busyMessage || ""),
      emptyMessage: String(handler.emptyMessage || ""),
      isActive: handler.isActive,
      submit: handler.submit,
    });
    composer.pageHandlers = [entry, ...composer.pageHandlers.filter((item) => !entry.id || item.id !== entry.id)];
    if (composer.root) renderEnterTarget();
    return () => {
      composer.pageHandlers = composer.pageHandlers.filter((item) => item !== entry);
      if (composer.root) renderEnterTarget();
    };
  }

  async function taskImagePayload() {
    const images = imageAttachments();
    if (images.length > TASK_IMAGE_MAX_COUNT) {
      throw new Error(`A task can include up to ${TASK_IMAGE_MAX_COUNT} images.`);
    }
    const out = [];
    let total = 0;
    for (const item of images) {
      let dataUrl = item.dataUrl;
      let parts = dataUrlParts(dataUrl);
      if (!parts || base64Size(parts.base64) > TASK_IMAGE_MAX_BYTES) {
        dataUrl = await shrinkImage(item.dataUrl, TASK_IMAGE_MAX_BYTES);
        parts = dataUrlParts(dataUrl);
      }
      if (!parts) throw new Error(`${item.name} is too large for a task image (256 KB).`);
      const size = base64Size(parts.base64);
      total += size;
      if (total > TASK_IMAGE_MAX_TOTAL_BYTES) throw new Error("Task images must total 1 MB or less.");
      const name = parts.mediaType === item.mediaType ? item.name : item.name.replace(/\.[^.]+$/, "") + ".jpg";
      out.push({ name, mime: parts.mediaType, size, file: parts.base64 });
    }
    return out;
  }

  async function createTask() {
    if (composer.busy || !validateBeforeSend("task")) return;
    setBusy(true);
    setStatus("Creating the task…");
    try {
      const { title, details } = splitTaskText(composer.els.input.value);
      const files = composer.attachments.filter((item) => item.kind === "text").map(attachmentText).join("");
      const context = activeContext();
      const contextLine = context && context.kind !== "repo" ? contextSuffix(context).trim() : "";
      const repo = activeRepo();
      const body = {
        title,
        details: [
          [details, files.trim()].filter(Boolean).join("\n\n").slice(0, TASK_DETAILS_MAX - (contextLine ? contextLine.length + 2 : 0)),
          contextLine,
        ].filter(Boolean).join("\n\n"),
        department: "general",
        destination: "department",
        assigneeKind: "unassigned",
        priority: 50,
      };
      if (repo) body.repository = repo.display;
      const attachments = await taskImagePayload();
      if (attachments.length) body.attachments = attachments;
      const payload = await api("POST", "/api/tasks", body);
      clearAfterSend();
      const message = "Task added to General.";
      setStatus(message, "good");
      setOutput(payload?.task?.title ? `${message} ${payload.task.title}` : message, { href: "/dashboard/tasks", label: "Open tasks" });
    } catch (error) {
      setStatus(String(error?.message || "The task could not be created."), "bad");
    } finally {
      setBusy(false);
    }
  }

  function builtinHelp() {
    return "Enter sends (new, or add when an agent session is open). Shift+Enter adds a line. Up/Down recall earlier prompts. /clear, /apps, /model <name>, /mode <auto|ask|plan|edit>, /speed <low|medium|high|ultra|max>, /agent <auto|claude|codex|gemma>.";
  }

  async function maybeRunSlashCommand() {
    const text = String(composer.els.input?.value || "").trim();
    const name = slashCommandName(text);
    if (!name) return false;
    const args = text.replace(/^\/\S+\s*/, "").trim().toLowerCase();
    if (name === "clear") {
      clearPrompt();
      return true;
    }
    if (name === "help") {
      setOutput(builtinHelp());
      composer.els.input.value = "";
      renderCount();
      return true;
    }
    if (name === "model") {
      const options = modelOptions(currentAgent(), activeRepoData()?.models);
      const matches = (option) => option.value === args || option.label.toLowerCase() === args || option.id === args;
      const match = options.find(matches);
      const other = match ? null : choiceItems("model").find((item) => {
        const [agent, value] = item.value.split("|");
        return matches(MODEL_CATALOG[agent].find((option) => option.value === value));
      });
      if (!match && !other) {
        setStatus(`Choose one of: ${options.map((option) => option.value).join(", ")}.`, "bad");
        return true;
      }
      if (match) setModel(match.value);
      else pickChoice("model", other.value);
      composer.els.input.value = "";
      renderCount();
      setStatus(`Model: ${(match || other).label}.`, "good");
      return true;
    }
    if (name === "agent") {
      const wanted = { claude: "claude-code", gemma: "local-llama" }[args] || args;
      if (!AGENTS.some((option) => option.value === wanted)) {
        setStatus("Choose auto, claude, codex or gemma.", "bad");
        return true;
      }
      setAgent(wanted);
      composer.els.input.value = "";
      renderCount();
      setStatus(`Agent: ${AGENTS.find((option) => option.value === wanted).label}.`, "good");
      return true;
    }
    if (name === "mode" || name === "speed") {
      const list = name === "mode" ? RUN_MODES : EFFORTS;
      const match = list.find((option) => option.value === args || option.label.toLowerCase() === args);
      if (!match) {
        setStatus(`Choose ${list.map((option) => option.label.toLowerCase()).join(", ")}.`, "bad");
        return true;
      }
      if (name === "mode") setRunMode(match.value);
      else setEffort(match.value);
      composer.els.input.value = "";
      renderCount();
      setStatus(`${name === "mode" ? "Mode" : "Speed"}: ${match.label}.`, "good");
      return true;
    }
    const commands = await loadAppCommands();
    if (name !== "apps" && !commands.some((entry) => entry.command === name)) return false;
    const apps = await loadChatApps();
    if (!apps) {
      setStatus("App commands could not be loaded.", "bad");
      return true;
    }
    setBusy(true);
    try {
      const handled = await apps.run(text, {
        channelId: "",
        showSystem: (message) => setOutput(String(message || "")),
        showEphemeral: (payload) => setOutput(apps.ephemeralNode(payload, {})),
      });
      if (handled) {
        rememberHistory(text);
        composer.els.input.value = "";
        setStored({ draft: "" });
        renderCount();
        setStatus(`Ran /${name}.`, "good");
      }
      return handled;
    } catch (error) {
      setStatus(String(error?.message || "The app command failed."), "bad");
      return true;
    } finally {
      setBusy(false);
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

  async function loadAppCommands() {
    if (Array.isArray(composer.appCommands)) return composer.appCommands;
    try {
      const payload = await api("GET", "/api/apps/commands");
      composer.appCommands = Array.isArray(payload?.commands) ? payload.commands : [];
    } catch (_) {
      composer.appCommands = [];
    }
    return composer.appCommands;
  }

  function setModel(value) {
    const provider = modeProvider(currentAgent());
    setStored({ models: { ...(composer.stored.models || {}), [provider]: value } });
    renderModels();
  }

  function setAgent(value) {
    setStored({ agent: value });
    renderModels();
    renderSessionsAndTargets();
  }

  function setAgentModel(agent, model) {
    setStored({ agent, models: { ...(composer.stored.models || {}), [agent]: model } });
    renderModels();
    renderSessionsAndTargets();
  }

  function setRunMode(value) {
    setStored({ runMode: value });
    renderChoice("mode");
  }

  function setEffort(value) {
    setStored({ effort: value });
    renderChoice("speed");
  }

  function clearPrompt() {
    const input = composer.els.input;
    if (!input) return;
    input.focus();
    input.select();
    let cleared = false;
    try {
      cleared = document.execCommand("delete");
    } catch (_) {}
    if (!cleared || input.value) input.value = "";
    setStored({ draft: "" });
    renderCount();
  }

  function insertAtCaret(text) {
    const input = composer.els.input;
    if (!input) return;
    const start = input.selectionStart ?? input.value.length;
    const end = input.selectionEnd ?? input.value.length;
    input.setRangeText(text, start, end, "end");
    input.dispatchEvent(new Event("input", { bubbles: true }));
    input.focus();
  }

  function menuItems() {
    const context = activeContext();
    const items = [
      { group: "Context", label: "Attach file…", hint: "photos, screenshots or text files", run: () => composer.els.file?.click() },
    ];
    if (context) {
      items.push({ group: "Context", label: "Insert page context", hint: context.label, run: () => insertAtCaret(`${context.label}: ${context.url}`) });
    }
    items.push(
      { group: "Context", label: "Clear prompt", hint: "/clear", run: clearPrompt },
      { group: "Context", label: "Rewind", hint: "previous prompt", run: () => recallHistory(-1) },
      { group: "Model", label: "Switch model…", hint: choiceItems("model").find((item) => item.value === choiceValue("model"))?.label || "", run: () => openChoice("model") },
      { group: "Model", label: "Switch permission mode…", hint: RUN_MODES.find((item) => item.value === currentRunMode())?.label || "", run: () => openChoice("mode") },
      { group: "Model", label: "Switch speed…", hint: EFFORTS.find((item) => item.value === currentEffort())?.label || "", run: () => openChoice("speed") },
      { group: "Model", label: composer.stored.createPr === true ? "Don't open a pull request" : "Open a pull request when done", hint: "dot in the header", run: togglePr },
      ...(canToggleCloud() ? [{ group: "Model", label: runsInCloud() ? "Run on your desktop" : "Run in the cloud", hint: "cloud", run: toggleCloud }] : []),
      { group: "Apps", label: "/apps", hint: "list your app commands", run: () => { composer.els.input.value = "/apps"; void maybeRunSlashCommand(); } },
    );
    for (const entry of composer.appCommands || []) {
      items.push({
        group: "Apps",
        label: `/${entry.command}`,
        hint: [entry.description, entry.app?.name ? `(${entry.app.name})` : ""].filter(Boolean).join(" "),
        run: () => {
          const input = composer.els.input;
          input.value = `/${entry.command} `;
          input.dispatchEvent(new Event("input", { bubbles: true }));
          input.focus();
          input.setSelectionRange(input.value.length, input.value.length);
        },
      });
    }
    const filter = composer.menu.filter.toLowerCase();
    return filter
      ? items.filter((item) => `${item.label} ${item.hint}`.toLowerCase().includes(filter))
      : items;
  }

  function renderMenu() {
    const menu = composer.els.menu;
    if (!menu) return;
    const items = menuItems();
    composer.menu.items = items;
    composer.menu.active = Math.max(0, Math.min(composer.menu.active, items.length - 1));
    let lastGroup = "";
    const parts = [];
    items.forEach((item, index) => {
      if (item.group !== lastGroup) {
        lastGroup = item.group;
        parts.push(`<div class="fmc-menu-group" role="presentation">${escapeHtml(item.group)}</div>`);
      }
      parts.push(`<div class="fmc-menu-item" role="option" id="fmcMenuItem${index}" data-composer-menu-item="${index}" aria-selected="${index === composer.menu.active}"><span>${escapeHtml(item.label)}</span><span class="fmc-menu-hint">${escapeHtml(item.hint || "")}</span></div>`);
    });
    if (!items.length) parts.push(`<div class="fmc-menu-empty">No matching command</div>`);
    menu.innerHTML = parts.join("");
    const activeId = items.length ? `fmcMenuItem${composer.menu.active}` : "";
    if (composer.menu.fromTyping) {
      if (activeId) composer.els.input.setAttribute("aria-activedescendant", activeId);
      else composer.els.input.removeAttribute("aria-activedescendant");
    } else if (activeId) {
      menu.setAttribute("aria-activedescendant", activeId);
    }
    menu.querySelector('[aria-selected="true"]')?.scrollIntoView({ block: "nearest" });
  }

  async function openMenu({ fromTyping = false, filter = "" } = {}) {
    composer.menu.open = true;
    composer.menu.fromTyping = fromTyping;
    composer.menu.filter = filter;
    composer.menu.active = 0;
    composer.els.menu.hidden = false;
    composer.els.slash.setAttribute("aria-expanded", "true");
    if (fromTyping) {
      composer.els.input.setAttribute("aria-controls", "fmcMenu");
      composer.els.input.setAttribute("aria-expanded", "true");
    }
    renderMenu();
    if (!fromTyping) composer.els.menu.focus();
    if (composer.appCommands === null) {
      await loadAppCommands();
      if (composer.menu.open) renderMenu();
    }
  }

  function closeMenu() {
    if (!composer.menu.open) return;
    composer.menu.open = false;
    composer.els.menu.hidden = true;
    composer.els.slash?.setAttribute("aria-expanded", "false");
    composer.els.input?.removeAttribute("aria-activedescendant");
    composer.els.input?.setAttribute("aria-expanded", "false");
  }

  function pickMenuItem(index) {
    const item = composer.menu.items[index];
    const fromTyping = composer.menu.fromTyping;
    closeMenu();
    if (!item) return;
    if (fromTyping && /^\/\S*$/.test(composer.els.input.value.trim()) && !item.label.startsWith("/")) {
      composer.els.input.value = "";
      renderCount();
    }
    item.run();
  }

  function menuKey(event) {
    if (!composer.menu.open) return false;
    const count = composer.menu.items.length;
    if (event.key === "ArrowDown" || event.key === "ArrowUp") {
      event.preventDefault();
      if (count) composer.menu.active = (composer.menu.active + (event.key === "ArrowDown" ? 1 : count - 1)) % count;
      renderMenu();
      return true;
    }
    if (event.key === "Enter" || event.key === "Tab") {
      if (!count) return false;
      event.preventDefault();
      pickMenuItem(composer.menu.active);
      return true;
    }
    if (event.key === "Escape") {
      event.preventDefault();
      event.stopPropagation();
      closeMenu();
      composer.els.input.focus();
      return true;
    }
    return false;
  }

  function syncTypedMenu() {
    const value = composer.els.input.value;
    const match = value.match(/^\/([a-z0-9-]*)$/i);
    if (match) {
      if (!composer.menu.open || !composer.menu.fromTyping) void openMenu({ fromTyping: true, filter: match[1] });
      else {
        composer.menu.filter = match[1];
        composer.menu.active = 0;
        renderMenu();
      }
    } else if (composer.menu.open && composer.menu.fromTyping) {
      closeMenu();
    }
  }

  function recallHistory(direction) {
    const history = Array.isArray(composer.stored.history) ? composer.stored.history : [];
    if (!history.length) {
      setStatus("No earlier prompts yet.");
      return false;
    }
    const input = composer.els.input;
    if (composer.historyIndex === -1) {
      if (direction > 0) return false;
      composer.historyScratch = input.value;
      composer.historyIndex = history.length - 1;
    } else {
      composer.historyIndex += direction;
    }
    if (composer.historyIndex >= history.length) {
      composer.historyIndex = -1;
      input.value = composer.historyScratch;
    } else {
      composer.historyIndex = Math.max(0, composer.historyIndex);
      input.value = history[composer.historyIndex];
    }
    input.focus();
    renderCount();
    scheduleDraftSave();
    return true;
  }

  function togglePr() {
    setStored({ createPr: composer.stored.createPr !== true });
    renderPr();
  }

  function toggleCloud() {
    if (!canToggleCloud()) return;
    setStored({ cloud: !runsInCloud() });
    renderCloud();
    renderSessionsAndTargets();
    setStatus(runsInCloud() ? "New agents run on a ForkMesh cloud runner." : "New agents run on your desktop.");
  }

  async function addFiles(files) {
    for (const file of Array.from(files || [])) {
      await addFile(file);
    }
    renderAttachments();
    scheduleDraftSave();
  }

  async function addFile(file) {
    if (!file) return;
    const type = String(file.type || "").toLowerCase();
    if (type.startsWith("image/")) {
      const images = imageAttachments();
      if (images.length >= AGENT_ATTACHMENT_MAX_COUNT) {
        setStatus(`Attach at most ${AGENT_ATTACHMENT_MAX_COUNT} images to one prompt.`, "bad");
        return;
      }
      let dataUrl;
      try {
        dataUrl = await readFileAs(file, "dataUrl");
      } catch (_) {
        setStatus(`Could not read ${file.name || "that image"}.`, "bad");
        return;
      }
      let parts = dataUrlParts(dataUrl);
      const used = images.reduce((sum, item) => sum + item.size, 0);
      const budget = Math.min(AGENT_ATTACHMENT_MAX_BYTES, AGENT_ATTACHMENT_MAX_TOTAL_BYTES - used);
      if (budget <= 0) {
        setStatus("The attached images already use the 10 MB limit.", "bad");
        return;
      }
      if (!parts || !IMAGE_TYPES.has(parts.mediaType) || base64Size(parts.base64) > budget) {
        setStatus(`Compressing ${file.name || "image"}…`);
        try {
          dataUrl = await shrinkImage(dataUrl, budget);
        } catch (_) {
          dataUrl = null;
        }
        parts = dataUrlParts(dataUrl);
        if (!parts) {
          setStatus(`${file.name || "That image"} could not be attached (JPEG, PNG, WebP or GIF up to 5 MB).`, "bad");
          return;
        }
      }
      const baseName = String(file.name || "screenshot.png").replace(/[\\/]/g, "_");
      const name = parts.mediaType === type ? baseName : baseName.replace(/\.[^.]+$/, "") + ".jpg";
      composer.attachments.push({
        id: `a${Date.now().toString(36)}${Math.random().toString(36).slice(2, 7)}`,
        kind: "image",
        name,
        mediaType: parts.mediaType,
        base64: parts.base64,
        dataUrl,
        size: base64Size(parts.base64),
      });
      setStatus(`Attached ${name}.`, "good");
      return;
    }
    if (isTextLike(file)) {
      if (file.size > TEXT_FILE_MAX_BYTES) {
        setStatus(`${file.name} is larger than ${formatBytes(TEXT_FILE_MAX_BYTES)}; paste the part the agent needs.`, "bad");
        return;
      }
      let text;
      try {
        text = String(await readFileAs(file, "text"));
      } catch (_) {
        setStatus(`Could not read ${file.name}.`, "bad");
        return;
      }
      const item = {
        id: `a${Date.now().toString(36)}${Math.random().toString(36).slice(2, 7)}`,
        kind: "text",
        name: String(file.name || "file.txt").replace(/[\\/]/g, "_"),
        text,
        size: file.size,
      };
      if (attachmentText(item).length > remainingChars()) {
        setStatus(`${item.name} does not fit in the ${MAX_PROMPT}-character prompt.`, "bad");
        return;
      }
      composer.attachments.push(item);
      setStatus(`Attached ${item.name}.`, "good");
      return;
    }
    setStatus(`${file.name || "That file"} can't be sent to an agent. Attach images or text files.`, "bad");
  }

  function speechRecognitionClass() {
    return window.SpeechRecognition || window.webkitSpeechRecognition || null;
  }

  function stopDictation() {
    if (composer.recognition) {
      try {
        composer.recognition.stop();
      } catch (_) {}
    }
  }

  function toggleDictation() {
    if (composer.listening) {
      stopDictation();
      return;
    }
    const Recognition = speechRecognitionClass();
    if (!Recognition) {
      setStatus("Dictation isn't available in this browser.", "bad");
      return;
    }
    const input = composer.els.input;
    const start = input.selectionStart ?? input.value.length;
    const end = input.selectionEnd ?? input.value.length;
    const before = input.value.slice(0, start);
    const after = input.value.slice(end);
    const spacer = before && !/\s$/.test(before) ? " " : "";
    let committed = "";
    const recognition = new Recognition();
    recognition.continuous = true;
    recognition.interimResults = true;
    recognition.lang = document.documentElement.lang || navigator.language || "en-US";
    recognition.onresult = (event) => {
      let interim = "";
      for (let index = event.resultIndex; index < event.results.length; index += 1) {
        const result = event.results[index];
        if (result.isFinal) committed += result[0].transcript;
        else interim += result[0].transcript;
      }
      input.value = (before + spacer + committed + interim + after).slice(0, MAX_PROMPT);
      const caret = Math.min(input.value.length, (before + spacer + committed + interim).length);
      input.setSelectionRange(caret, caret);
      renderCount();
    };
    recognition.onerror = (event) => {
      const code = String(event?.error || "");
      setStatus(code === "not-allowed" || code === "service-not-allowed"
        ? "Microphone access was denied."
        : code === "no-speech" ? "No speech was heard." : "Dictation stopped.", "bad");
    };
    recognition.onend = () => {
      composer.listening = false;
      composer.recognition = null;
      composer.els.mic.setAttribute("aria-pressed", "false");
      composer.els.mic.dataset.listening = "0";
      input.value = (before + spacer + committed + after).slice(0, MAX_PROMPT);
      renderCount();
      scheduleDraftSave();
    };
    try {
      recognition.start();
    } catch (_) {
      setStatus("Dictation could not start.", "bad");
      return;
    }
    composer.recognition = recognition;
    composer.listening = true;
    composer.els.mic.setAttribute("aria-pressed", "true");
    composer.els.mic.dataset.listening = "1";
    setStatus("Listening… click the microphone again to stop.");
  }

  function onInputKeydown(event) {
    if (menuKey(event)) return;
    if (event.isComposing) return;
    if (event.key === "Enter" && !event.shiftKey && !event.altKey) {
      event.preventDefault();
      event.stopPropagation();
      const action = enterAction();
      if (action === "page") void runPageAction();
      else if (action === "add") void addToSession();
      else void startNewSession();
      return;
    }
    if (event.key === "ArrowUp" && !event.shiftKey && composer.els.input.selectionStart === 0 && composer.els.input.selectionEnd === 0) {
      if (recallHistory(-1)) event.preventDefault();
      return;
    }
    if (event.key === "ArrowDown" && !event.shiftKey && composer.historyIndex !== -1 &&
        composer.els.input.selectionStart === composer.els.input.value.length) {
      if (recallHistory(1)) event.preventDefault();
      return;
    }
    if (event.key === "Escape") {
      event.preventDefault();
      setOpen(false);
    }
  }

  function startDrag(event) {
    if (composer.inline || event.button !== 0 || !window.matchMedia("(min-width: 640px)").matches) return;
    const panel = composer.els.panel;
    const rect = panel.getBoundingClientRect();
    composer.drag = {
      id: event.pointerId,
      startX: event.clientX,
      startY: event.clientY,
      right: window.innerWidth - rect.right,
      bottom: window.innerHeight - rect.bottom,
    };
    composer.els.handle.setPointerCapture?.(event.pointerId);
    event.preventDefault();
  }

  function moveDrag(event) {
    const drag = composer.drag;
    if (!drag || drag.id !== event.pointerId) return;
    const panel = composer.els.panel;
    const rect = panel.getBoundingClientRect();
    const right = Math.max(8, Math.min(drag.right - (event.clientX - drag.startX), window.innerWidth - rect.width - 8));
    const bottom = Math.max(8, Math.min(drag.bottom - (event.clientY - drag.startY), window.innerHeight - rect.height - 8));
    panel.style.right = right + "px";
    panel.style.bottom = bottom + "px";
    drag.last = { right: Math.round(right), bottom: Math.round(bottom) };
  }

  function endDrag(event) {
    const drag = composer.drag;
    if (!drag || drag.id !== event.pointerId) return;
    composer.drag = null;
    if (drag.last) setStored({ position: drag.last });
  }

  function resetPlacement() {
    setStored({ position: null, large: false });
    applyPlacement();
  }

  function wire() {
    const els = composer.els;
    els.pill.addEventListener("click", () => setOpen(true));
    els.collapse.addEventListener("click", () => setOpen(false));
    els.avatar.addEventListener("click", () => setOpen(false));
    els.clear.addEventListener("click", clearPrompt);
    els.pr.addEventListener("click", togglePr);
    els.cloud.addEventListener("click", toggleCloud);
    els.reset.addEventListener("click", () => {
      resetPlacement();
      composer.appCommands = null;
      refreshContext();
      void ensureRepoChoices();
      void loadRepoData({ force: true });
      setStatus("Position reset; sessions and nodes refreshed.");
    });
    els.expand.addEventListener("click", () => {
      setStored({ large: !isExpanded() });
      applyPlacement();
      els.input.focus();
    });
    els.handle.addEventListener("pointerdown", startDrag);
    els.handle.addEventListener("pointermove", moveDrag);
    els.handle.addEventListener("pointerup", endDrag);
    els.handle.addEventListener("pointercancel", endDrag);
    els.handle.addEventListener("dblclick", resetPlacement);
    els.chipRemove.addEventListener("click", () => {
      composer.dismissedContextKey = composer.context?.key || "";
      renderContext();
      renderModels();
      renderSessionsAndTargets();
      renderCount();
      renderEnterTarget();
      void loadRepoData();
      els.repoSelect.focus();
    });
    els.repoSelect.addEventListener("change", () => {
      setStored({ repo: els.repoSelect.value });
      renderModels();
      renderSessionsAndTargets();
      renderEnterTarget();
      void loadRepoData();
    });
    els.sessionSelect.addEventListener("change", renderGauge);
    for (const key of ["model", "mode", "speed"]) {
      const button = els[key];
      const menu = $(`[data-composer-choice-menu="${key}"]`);
      button.addEventListener("click", () => {
        if (composer.choice.open === key) closeChoice();
        else openChoice(key);
      });
      button.addEventListener("keydown", (event) => {
        if (event.key === "ArrowDown" || event.key === "ArrowUp") {
          event.preventDefault();
          openChoice(key);
        }
      });
      menu.addEventListener("keydown", (event) => onChoiceKeydown(event, key));
      menu.addEventListener("mousedown", (event) => event.preventDefault());
      menu.addEventListener("click", (event) => {
        const option = event.target.closest("[data-composer-choice-option]");
        if (!option) return;
        const item = choiceItems(key)[Number(option.dataset.composerChoiceOption)];
        closeChoice();
        if (item) pickChoice(key, item.value);
      });
    }
    els.input.addEventListener("input", () => {
      composer.historyIndex = -1;
      renderCount();
      scheduleDraftSave();
      syncTypedMenu();
    });
    els.input.addEventListener("keydown", onInputKeydown);
    els.input.addEventListener("focus", refreshContext);
    els.input.addEventListener("paste", (event) => {
      const files = Array.from(event.clipboardData?.items || [])
        .filter((item) => item.kind === "file")
        .map((item) => item.getAsFile())
        .filter(Boolean);
      if (!files.length) return;
      event.preventDefault();
      void addFiles(files);
    });
    els.panel.addEventListener("dragover", (event) => {
      if (Array.from(event.dataTransfer?.types || []).includes("Files")) {
        event.preventDefault();
        els.panel.dataset.dropping = "1";
      }
    });
    els.panel.addEventListener("dragleave", () => { els.panel.dataset.dropping = "0"; });
    els.panel.addEventListener("drop", (event) => {
      if (!event.dataTransfer?.files?.length) return;
      event.preventDefault();
      els.panel.dataset.dropping = "0";
      void addFiles(event.dataTransfer.files);
    });
    els.task.addEventListener("click", () => void createTask());
    els.add.addEventListener("click", () => void addToSession());
    els.create.addEventListener("click", () => void startNewSession());
    els.page.addEventListener("click", () => void runPageAction());
    els.attach.addEventListener("click", () => els.file.click());
    els.file.addEventListener("change", () => {
      const files = Array.from(els.file.files || []);
      els.file.value = "";
      void addFiles(files);
    });
    els.attachments.addEventListener("click", (event) => {
      const button = event.target.closest("[data-composer-attachment-remove]");
      if (!button) return;
      composer.attachments = composer.attachments.filter((item) => item.id !== button.dataset.composerAttachmentRemove);
      renderAttachments();
      els.input.focus();
    });
    if (!speechRecognitionClass()) {
      els.mic.disabled = true;
      els.mic.title = "Dictation isn't available in this browser";
      els.mic.setAttribute("aria-label", "Dictation isn't available in this browser");
    }
    els.mic.addEventListener("click", toggleDictation);
    els.slash.addEventListener("click", () => {
      if (composer.menu.open) {
        closeMenu();
        els.input.focus();
      } else if (!els.input.value.trim()) {
        els.input.value = "/";
        els.input.focus();
        els.input.setSelectionRange(1, 1);
        renderCount();
        void openMenu({ fromTyping: true });
      } else {
        void openMenu();
      }
    });
    els.menu.addEventListener("keydown", (event) => {
      if (menuKey(event)) return;
      if (event.key.length === 1 && !event.ctrlKey && !event.metaKey && !event.altKey) {
        composer.menu.filter += event.key;
        composer.menu.active = 0;
        renderMenu();
        event.preventDefault();
      } else if (event.key === "Backspace") {
        composer.menu.filter = composer.menu.filter.slice(0, -1);
        renderMenu();
        event.preventDefault();
      }
    });
    els.menu.addEventListener("mousedown", (event) => event.preventDefault());
    els.menu.addEventListener("click", (event) => {
      const option = event.target.closest("[data-composer-menu-item]");
      if (option) pickMenuItem(Number(option.dataset.composerMenuItem));
    });
    document.addEventListener("pointerdown", (event) => {
      if (composer.menu.open && !event.target.closest("[data-composer-menu], [data-composer-slash]")) closeMenu();
      if (composer.choice.open && !event.target.closest(`[data-composer-choice="${composer.choice.open}"]`)) closeChoice({ focus: false });
    }, true);
    document.addEventListener("keydown", (event) => {
      if (event.key === "/" && (event.ctrlKey || event.metaKey) && !event.altKey && !event.shiftKey) {
        event.preventDefault();
        if (isOpen() && document.activeElement === els.input) setOpen(false);
        else if (isOpen()) els.input.focus();
        else setOpen(true);
      }
    });
    document.addEventListener("click", () => {
      if (isOpen()) window.setTimeout(refreshContext, 0);
    });
    window.addEventListener("popstate", refreshContext);
    window.addEventListener("resize", () => {
      applyViewport();
      if (isOpen()) applyPlacement();
    });
    window.visualViewport?.addEventListener("resize", applyViewport);
    window.visualViewport?.addEventListener("scroll", applyViewport);
    applyViewport();
  }

  function applyViewport() {
    const root = composer.root;
    if (!root) return;
    const viewport = window.visualViewport;
    if (!viewport || viewport.scale > 1.01 || !window.matchMedia("(max-width: 639px)").matches) {
      root.style.removeProperty("--fmc-keyboard");
      root.style.removeProperty("--fmc-viewport");
      delete root.dataset.keyboard;
      return;
    }
    const keyboard = Math.max(0, Math.round(window.innerHeight - viewport.height - viewport.offsetTop));
    root.style.setProperty("--fmc-keyboard", keyboard + "px");
    root.style.setProperty("--fmc-viewport", Math.round(viewport.height) + "px");
    if (keyboard > 0) root.dataset.keyboard = "1";
    else delete root.dataset.keyboard;
  }

  function shouldMount() {
    if (document.documentElement.dataset.worldEmbed === "1") return false;
    if (window.top !== window.self) return false;
    return isUserSession(readSession());
  }

  function mount() {
    if (composer.root || !document.body || !shouldMount()) return;
    composer.session = readSession();
    composer.account = accountKey(composer.session);
    composer.stored = loadStored();
    composer.root = buildMarkup();
    const slot = document.querySelector("[data-composer-slot]");
    composer.inline = Boolean(slot);
    if (slot) {
      composer.root.dataset.inline = "1";
      slot.hidden = false;
      slot.replaceChildren(composer.root);
    } else {
      document.body.append(composer.root);
    }
    bindElements();
    composer.els.input.value = String(composer.stored.draft || "").slice(0, MAX_PROMPT);
    composer.context = detectContext(composer.session);
    wire();
    renderAll();
    const idle = !document.activeElement || document.activeElement === document.body;
    const startOpen = composer.inline || Boolean(composer.stored.open);
    setOpen(startOpen, { focus: idle && startOpen });
  }

  function releaseSlot() {
    const slot = document.querySelector("[data-composer-slot]");
    if (slot && !slot.querySelector("[data-composer]")) slot.hidden = true;
  }

  window.ForkMeshComposer = Object.freeze({
    open: () => setOpen(true),
    close: () => setOpen(false),
    toggle: () => setOpen(!isOpen()),
    registerPageHandler,
    insert: (text) => {
      setOpen(true);
      insertAtCaret(String(text || ""));
    },
    state: () => ({
      open: isOpen(),
      inline: composer.inline,
      account: composer.account,
      agent: currentAgent(),
      mode: currentRunMode(),
      effort: currentEffort(),
      model: currentModel(),
      repo: activeRepo()?.display || "",
      context: activeContext()?.label || "",
      enterAction: enterAction(),
      page: pageHandler()?.id || "",
      attachments: composer.attachments.map((item) => ({ kind: item.kind, name: item.name, size: item.size })),
      remaining: remainingChars(),
    }),
    _internals: Object.freeze({
      parseRepoPath,
      splitTaskText,
      slashCommandName,
      modelOptions,
      pickTarget,
      eligibleTargets,
      agentPlacement,
      placementNotice,
      withOwnerRoute,
      promptableSessions,
      contextUsage,
      contextSuffix,
      isTextLike,
      repoFromDisplay,
      MAX_PROMPT,
      TASK_TITLE_MAX,
      TASK_DETAILS_MAX,
      AGENT_ATTACHMENT_MAX_COUNT,
      AGENT_ATTACHMENT_MAX_BYTES,
      AGENT_ATTACHMENT_MAX_TOTAL_BYTES,
      TASK_IMAGE_MAX_COUNT,
      TASK_IMAGE_MAX_BYTES,
      TASK_IMAGE_MAX_TOTAL_BYTES,
      MODEL_CATALOG,
      DEFAULT_MODEL,
      RUN_MODES,
      EFFORTS,
    }),
  });

  window.dispatchEvent(new Event("forkmesh:composer-ready"));

  function hostReady() {
    if (!composer.root) {
      mount();
      if (!composer.root) releaseSlot();
      return;
    }
    composer.session = readSession() || composer.session;
    refreshContext();
    renderAvatar();
    if (isOpen()) void ensureRepoChoices();
  }

  function start() {
    if (document.documentElement.dataset.composerHost === "1") hostReady();
    else mount();
  }

  window.addEventListener("forkmesh:composer-host", hostReady);

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", start, { once: true });
  } else {
    start();
  }
})();

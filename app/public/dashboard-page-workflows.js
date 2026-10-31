// 09-workflows.js, loaded by dashboard.js when its page runs (app/src/dashboard_bundle.py LAZY_CHUNKS).
(() => {
  const chunks = window.__forkmeshDashboardChunks = window.__forkmeshDashboardChunks || {};
  chunks["workflows"] = (core) => {
  const { $, $$, blobText, desktopBrand, desktopControlErrorText, desktopControlHub, escapeHtml, fetchJson, fetchRepoJson, formatTimeAgo, platformName, sendTextToPrompt, state } = core;
  const WF_NODE_W = 216;
  const WF_NODE_H = 64;
  const WF_GRID = 8;
  const WF_SVG_OFFSET = 20000;
  const WF_COLUMN = 288;
  const WF_ROW = 120;
  const WF_LONG_PRESS_MS = 550;
  const WF_ERROR_TEXT = {
    unauthorized: "Log in with a user account to build workflows.",
    not_found: "That workflow no longer exists.",
    invalid_workflow: "The workflow could not be read. Reload the page and try again.",
    invalid_json: "The request could not be read. Reload the page and try again.",
    too_many_nodes: "A workflow can have at most 60 steps.",
    too_many_connections: "A workflow can have at most 120 connections.",
    invalid_node: "One of the steps is not valid.",
    unknown_node_type: "One of the steps uses an integration that no longer exists.",
    duplicate_node_name: "Every step needs its own name.",
    value_too_long: "A field value is longer than 20,000 characters.",
    invalid_credential: "One of the steps points at a credential that is not valid.",
    too_many_triggers: "A workflow can have only one trigger.",
    invalid_connection: "One of the connections is not valid.",
    cycle: "Connections cannot loop back to an earlier step.",
    too_many_workflows: "You already have 50 workflows. Delete one to add another.",
    no_trigger: "Add a trigger before running the workflow.",
    rate_limited: "This account ran more than 120 workflows in the last hour. Try again later.",
    unknown_integration: "Pick an integration for the credential.",
    missing_credential_field: "Fill in every credential field.",
    invalid_credential_url: "Credential URLs must be public https:// addresses.",
    too_many_credentials: "You already have 50 credentials. Delete one to add another.",
    no_credential_needed: "This integration does not need a credential.",
    document_too_large: "That file is larger than 1 MB.",
    script_unavailable: "This server does not have the script's source. The command above is what the desktop runs.",
    invalid_document: "That file is not valid JSON.",
    not_a_workflow: "That file does not contain a workflow (no nodes and connections).",
    invalid_import_url: "Use a public https:// link to a workflow JSON file.",
    import_fetch_failed: "That link could not be downloaded.",
    yaml_syntax: `The file is not valid YAML for ${platformName()} Actions.`,
    not_a_mapping: "The action file must be a YAML mapping.",
    no_steps: "An action needs at least one step with a command, connected to the trigger.",
    too_many_steps: "An action can have at most 64 steps.",
    too_many_events: "An action can run on at most 8 events.",
    empty_step: "Every connected step needs a command.",
    invalid_job_id: "Every job needs its own id made of letters, numbers, dots, dashes or underscores.",
    run_first_line_indented: "A command's first line cannot be indented more than the lines after it.",
    env_not_json: "Environment must be a JSON object.",
    invalid_fetch_depth: "Fetch depth must be 0 or more.",
    invalid_json_file: "The special agent file is not valid JSON.",
    invalid_agent_id: "The agent id may use lowercase letters, numbers and dashes.",
    agent_id_mismatch: "The agent id must match its file name.",
    agent_needs_name_and_specialty: "A special agent needs a name and a specialty.",
    unknown_permission: "One of the permissions is not known to the desktop.",
    invalid_run_window: "The run window is 0 to 720 hours.",
    invalid_schedule_mode: "Pick how often the agent runs.",
    invalid_schedule_time: "Use a time like 09:00.",
    invalid_schedule_days: "Pick days from Monday to Sunday.",
    invalid_event: "One of the events is not known to the desktop.",
    monitor_needs_badge: "Every log monitor needs a badge.",
    too_many_monitors: "A special agent can have at most 16 log monitors.",
    invalid_skill_ref: "Every skill file needs a scope and a file name.",
    invalid_native_workflow: "Pick a native workflow the desktop knows.",
    sandbox_needs_manual_run: "Steps outside the sandbox run only by hand: keep Manual run as the only event.",
    missing_root: "The file needs its starting step.",
    missing_profile: "A special agent needs its profile step.",
    unsupported_path: "That file type cannot be edited here.",
    file_too_large: "The file is larger than 256 KB.",
    forbidden: "You need write access to this repository to save.",
    conflict: "The file changed in the repository while you were editing. Reload it and try again.",
    plugin_not_installed: "This part of Workflows is turned off for your account.",
    runs_on_desktop: `This workflow runs on your desktop. Press Run workflow here while the ${desktopBrand()}desktop app is open.`,
    desktop_steps_only: `Desktop steps cannot be mixed with steps that run on ${platformName()}. Keep them in their own workflow.`,
    desktop_needs_manual: "Desktop steps run only by hand: start the workflow with Manual run.",
    not_a_desktop_workflow: "Only workflows with desktop steps record runs from the desktop.",
    invalid_run: "The desktop's run could not be recorded.",
  };
  const WF_DESKTOP_STEP_TIMEOUT_MS = 20 * 60 * 1000 + 30 * 1000;
  const WF_DESKTOP_ERRORS = {
    busy: "Another disk cleanup step is still running on the desktop.",
    bad_step: "The desktop does not know this step.",
    unsupported_action: `The desktop app is too old for desktop steps. Update the ${desktopBrand()}desktop app.`,
    cleanup_failed: "The cleanup step failed on the desktop.",
  };
  const WF_FAILURE_PROMPT_MAX = 7500;
  const WF_FAILURE_LOG_LINES = 40;
  const WF_FAILURE_PARAMS_MAX = 1500;
  const WF_SECRET_KEY_RE = /token|secret|password|passwd|api[_-]?key|auth|cookie|signature/i;
  const WF_DESKTOP_SCRIPT = "/api/workflows/desktop-script";
  const WF_SCRIPT_EXTENSIONS = [".sh", ".bash"];
  const WF_BODY_METHODS = ["POST", "PUT", "PATCH", "DELETE"];
  const WF_SCRIPT_TAGS = { script: "Script", "script-part": "Part of script" };
  const WF_PACKAGE = "workflows";
  const WF_EXAMPLES_PACKAGE = "workflows-examples";
  const WF_FILES_PACKAGE = "workflows-repository-files";
  const WF_CATEGORY_ICONS = {
    "Communication": "message-square",
    "Email": "mail",
    "Productivity": "layout-grid",
    "Forms and meetings": "calendar",
    "Developer tools": "code",
    "CRM and support": "users",
    "Commerce and finance": "shopping-cart",
    "Social and content": "share-2",
    "Analytics": "activity",
    "AI": "sparkles",
    "Data and storage": "database",
    "Utilities": "wrench",
  };
  const WF_CORE_GROUPS = ["Triggers", "Core", "Flow"];
  const WF_OPERATOR_SYMBOLS = {
    equals: "=", notEquals: "≠", contains: "contains", notContains: "lacks",
    startsWith: "starts with", endsWith: "ends with", gt: ">", gte: "≥", lt: "<", lte: "≤",
    isEmpty: "is empty", isNotEmpty: "is set", isTrue: "is true", isFalse: "is false",
    matches: "matches", notMatches: "does not match",
  };
  const WF_OUTPUT_LABELS = { case1: "1", case2: "2", case3: "3", otherwise: "else" };
  const WF_SELECT_LABELS = (spec, key, value) => {
    const field = (spec.fields || []).find((item) => item.key === key);
    const option = (field?.options || []).find((item) => String(item.value) === String(value ?? field?.default ?? ""));
    return option ? option.label : String(value ?? field?.default ?? "");
  };
  const WF_DAYS = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];

  function wfView() {
    return state.workflowsView;
  }

  function wfAccountName() {
    const session = state.session;
    if (!session?.nodeName || session.kind === "node") return "";
    if (session.kind !== "user" && !session.email) return "";
    return String(session.nodeName).toLowerCase();
  }

  function wfErrorText(error, fallback) {
    return WF_ERROR_TEXT[String(error?.code || error?.message || "")] || fallback;
  }

  function wfAuthHeaders(json = true) {
    const headers = { accept: "application/json" };
    if (json) headers["content-type"] = "application/json";
    const token = state.session?.sessionToken || "";
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    return headers;
  }

  async function wfPost(path, body) {
    const response = await fetch(path, {
      method: "POST",
      headers: wfAuthHeaders(),
      cache: "no-store",
      credentials: "same-origin",
      body: JSON.stringify(body || {}),
    });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(payload.error || `HTTP ${response.status}`);
      error.status = response.status;
      error.code = payload.error || "";
      throw error;
    }
    return payload;
  }

  function wfPluginOn(id) {
    const plugins = wfView().plugins;
    if (!plugins || !plugins.has(id)) return true;
    return plugins.get(id) === true;
  }

  function wfPackFor(appKey) {
    return (wfView().catalog?.packs || []).find((pack) => pack.integrations.includes(appKey)) || null;
  }

  function wfAppOn(app) {
    const pack = wfPackFor(app.key);
    return !pack || wfPluginOn(pack.id);
  }

  async function wfLoadPlugins() {
    const view = wfView();
    try {
      const data = await fetchJson("/api/plugins/@account", { fresh: true });
      const rows = Array.isArray(data.packages) ? data.packages : [];
      view.plugins = rows.length ? new Map(rows.map((row) => [String(row.id), row.installed === true])) : null;
    } catch {
      view.plugins = null;
    }
  }

  async function wfSetPlugin(id, on) {
    const view = wfView();
    try {
      const data = await wfPost(`/api/plugins/@account/${encodeURIComponent(id)}/${on ? "install" : "uninstall"}`, {});
      if (Array.isArray(data.packages)) view.plugins = new Map(data.packages.map((row) => [String(row.id), row.installed === true]));
      else await wfLoadPlugins();
      return true;
    } catch (error) {
      wfSetNotice(wfErrorText(error, "That package could not be changed right now."), "bad");
      return false;
    }
  }

  function wfIsSmall() {
    return window.matchMedia("(max-width: 1023px)").matches;
  }

  function wfIsFileMode() {
    return wfView().current?.mode === "file";
  }

  function wfCatalogApps() {
    return wfView().catalog?.integrations || [];
  }

  function wfApp(type) {
    if (!String(type || "").startsWith("app.")) return null;
    return wfCatalogApps().find((app) => app.type === type) || null;
  }

  function wfCore(type) {
    return (wfView().catalog?.core || []).find((item) => item.type === type) || null;
  }

  function wfFileSpec(type) {
    return (wfView().catalog?.files?.nodes || []).find((item) => item.type === type) || null;
  }

  function wfSpec(type) {
    const core = wfCore(type);
    if (core) {
      return {
        kind: "core", type, name: core.name, color: core.color, icon: core.icon,
        outputs: core.outputs || ["main"], inputs: !core.trigger, trigger: Boolean(core.trigger),
        single: Boolean(core.trigger), group: core.group, description: core.description, fields: core.fields || [],
      };
    }
    const app = wfApp(type);
    if (app) {
      return {
        kind: "app", type, name: app.name, color: app.color, icon: "", outputs: ["main"], inputs: true,
        trigger: false, single: false, group: app.category, description: app.description, app,
      };
    }
    const file = wfFileSpec(type);
    if (file) {
      return {
        kind: "file", type, name: file.name, color: file.color, icon: file.icon,
        outputs: file.outputs || [], inputs: file.inputs !== false, trigger: file.inputs === false,
        single: Boolean(file.single), group: file.group, description: file.description, fields: file.fields || [],
        fileKind: file.kind,
      };
    }
    return {
      kind: "unknown", type, name: type, color: "#64748b", icon: "circle-help", outputs: ["main"],
      inputs: true, trigger: false, single: false, group: "", description: "", fields: [],
    };
  }

  function wfIsTrigger(type) {
    return wfSpec(type).trigger;
  }

  function wfIsDesktopType(type) {
    return wfCore(type)?.runsOn === "desktop";
  }

  function wfHasDesktopSteps(workflow) {
    return Boolean(workflow?.nodes?.some((node) => wfIsDesktopType(node.type)));
  }

  function wfRunOrder(workflow) {
    const incoming = new Map(workflow.nodes.map((node) => [node.id, 0]));
    for (const conn of workflow.connections) incoming.set(conn.to, (incoming.get(conn.to) || 0) + 1);
    const queue = workflow.nodes.filter((node) => !incoming.get(node.id)).sort((a, b) => a.x - b.x).map((node) => node.id);
    const order = [];
    while (queue.length) {
      const id = queue.shift();
      order.push(id);
      for (const conn of workflow.connections) {
        if (conn.from !== id) continue;
        incoming.set(conn.to, incoming.get(conn.to) - 1);
        if (incoming.get(conn.to) === 0) queue.push(conn.to);
      }
    }
    return order.map((id) => workflow.nodes.find((node) => node.id === id)).filter(Boolean);
  }

  function wfDuration(ms) {
    const value = Math.max(0, Math.round(Number(ms) || 0));
    if (value < 1000) return `${value} ms`;
    const seconds = Math.round(value / 1000);
    if (seconds < 60) return `${seconds} s`;
    const minutes = Math.floor(seconds / 60);
    if (minutes < 60) return `${minutes} m ${String(seconds % 60).padStart(2, "0")} s`;
    return `${Math.floor(minutes / 60)} h ${String(minutes % 60).padStart(2, "0")} m`;
  }

  function wfBytes(size) {
    let value = Math.max(0, Number(size) || 0);
    for (const unit of ["B", "KB", "MB", "GB"]) {
      if (value < 1024) return unit === "B" ? `${Math.round(value)} B` : `${value.toFixed(1)} ${unit}`;
      value /= 1024;
    }
    return `${value.toFixed(1)} TB`;
  }

  function wfOutputs(type) {
    return wfSpec(type).outputs;
  }

  function wfOperation(node) {
    const app = wfApp(node.type);
    if (!app) return null;
    return app.operations.find((op) => op.key === node.params?.operation) || app.operations[0] || null;
  }

  function wfFields(node) {
    const spec = wfSpec(node.type);
    if (spec.kind === "app") return wfOperation(node)?.fields || [];
    return spec.fields || [];
  }

  function wfInitials(name) {
    const words = String(name || "?").replace(/[^A-Za-z0-9 ]/g, " ").trim().split(/\s+/).filter(Boolean);
    if (!words.length) return "?";
    if (words.length === 1) return words[0].slice(0, 2).toUpperCase();
    return (words[0][0] + words[1][0]).toUpperCase();
  }

  function wfBadge(type, small = false) {
    const spec = wfSpec(type);
    const cls = small ? "wf-badge wf-badge-sm" : "wf-badge";
    const inner = spec.icon
      ? `<i data-lucide="${escapeHtml(spec.icon)}"></i>`
      : escapeHtml(wfInitials(spec.name));
    return `<span class="${cls}" style="background:${escapeHtml(spec.color || "#64748b")}">${inner}</span>`;
  }

  function wfNodeId() {
    const bytes = new Uint8Array(6);
    crypto.getRandomValues(bytes);
    return "n" + Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
  }

  function wfUniqueName(base) {
    const names = new Set((wfView().current?.nodes || []).map((node) => node.name));
    if (!names.has(base)) return base;
    for (let i = 2; ; i += 1) {
      if (!names.has(`${base} ${i}`)) return `${base} ${i}`;
    }
  }

  function wfNode(id) {
    return (wfView().current?.nodes || []).find((node) => node.id === id) || null;
  }

  function wfShort(text, length = 48) {
    const clean = String(text ?? "").replace(/\s+/g, " ").trim();
    return clean.length > length ? `${clean.slice(0, length - 1)}…` : clean;
  }

  function wfFirstLine(text) {
    return String(text || "").split("\n").map((line) => line.trim()).find(Boolean) || "";
  }

  const WF_STEP_RE = /^#\s*step:\s*(.+?)\s*$/;
  const WF_RUN_SHORTCUT_RE = /^\s*bash\s+\.forkmesh\/shortcuts\/([A-Za-z0-9._-]{1,96})(?:\s|$)/;

  function wfStepName(text) {
    const first = String(text || "").split("\n").find((line) => line.trim());
    const match = first ? first.match(WF_STEP_RE) : null;
    return match ? match[1].replace(/\s+/g, " ").slice(0, 80) : "";
  }

  function wfStepCommand(text) {
    const lines = String(text || "").split("\n").map((line) => line.trim()).filter(Boolean);
    if (lines.length && wfStepName(text)) lines.shift();
    return lines[0] || "";
  }

  function wfShortcutLink(node) {
    if (node.type !== "shortcut.run") return "";
    for (const line of String(node.params?.text || "").split("\n")) {
      const match = line.match(WF_RUN_SHORTCUT_RE);
      if (match) return match[1];
    }
    return "";
  }

  function wfList(value) {
    if (Array.isArray(value)) return value.map(String);
    return String(value || "").split(/\r?\n/).map((item) => item.trim()).filter(Boolean);
  }

  function wfChecks(value) {
    if (Array.isArray(value)) return value.map(String);
    const text = String(value || "").trim();
    if (!text) return [];
    if (text.startsWith("[")) {
      try {
        const parsed = JSON.parse(text);
        if (Array.isArray(parsed)) return parsed.map(String);
      } catch {
        return [];
      }
    }
    return text.split(",").map((item) => item.trim()).filter(Boolean);
  }

  function wfHost(url) {
    try {
      return new URL(String(url || "").replace(/\{\{.*?\}\}/g, "x")).host;
    } catch {
      return wfShort(url, 30);
    }
  }

  function wfNodeTitle(node) {
    const p = node.params || {};
    switch (node.type) {
      case "action.trigger": return p.name || "Action";
      case "action.job": return `Job ${p.id || ""}`.trim();
      case "action.step": return p.name || wfShort(wfFirstLine(p.run), 40) || "Run step";
      case "shortcut.header":
      case "skill.header": return p.name || wfShort(String(wfView().current?.path || "").split("/").pop(), 40);
      case "shortcut.block":
      case "shortcut.run": return wfShort(wfStepName(p.text), 60) || node.name;
      case "agent.profile": return p.name || "Special agent";
      default: return node.name;
    }
  }

  function wfNodeSummary(node) {
    const p = node.params || {};
    const spec = wfSpec(node.type);
    switch (node.type) {
      case "core.manual": return "Runs when you press Run";
      case "core.webhook": return "Runs when its URL is called";
      case "core.schedule": return `Every ${p.every || 60} minutes`;
      case "core.http": return `${p.method || "GET"} ${wfHost(p.url)}`;
      case "core.set": {
        let keys = [];
        try {
          keys = Object.keys(typeof p.values === "string" ? JSON.parse(p.values || "{}") : (p.values || {}));
        } catch {
          keys = [];
        }
        return keys.length ? `Sets ${keys.slice(0, 3).join(", ")}${keys.length > 3 ? "…" : ""}` : "Sets fields";
      }
      case "core.if":
      case "core.filter": {
        const left = String(p.left || "").replace(/\{\{\s*\$json\.?|\s*\}\}/g, "") || "value";
        const op = WF_OPERATOR_SYMBOLS[p.operator || "equals"] || p.operator;
        const right = ["isEmpty", "isNotEmpty", "isTrue", "isFalse"].includes(p.operator) ? "" : ` ${p.right || ""}`;
        return wfShort(`${left} ${op}${right}`);
      }
      case "core.merge": return "Combines incoming branches";
      case "core.limit": return `Keeps the first ${p.max || 10}`;
      case "core.split": return `One item per ${p.field || "entry"}`;
      case "core.noop": return "Passes items through";
      case "core.code": return wfShort(wfFirstLine(String(p.code || "").split("\n").filter((line) => !line.trim().startsWith("//")).join("\n"))) || "Script";
      case "core.variable": return `${p.mode === "append" ? "Adds to" : p.mode === "add" ? "Adds up" : "Sets"} $vars.${p.name || "name"}`;
      case "core.switch": return wfShort(["case1", "case2", "case3"].map((key) => p[key]).filter(Boolean).join(" · ")) || "Choose a branch";
      case "core.repeat": return `${p.times || 3} times`;
      case "core.wait": return `Waits ${p.seconds || 5} s`;
      case "core.runWorkflow": return wfShort(p.workflow) || "Choose a workflow";
      case "core.stop": return p.outcome === "error" ? wfShort(p.message) || "Stops with an error" : "Stops the run";
      case "core.comment": return wfShort(wfFirstLine(p.text)) || "Note";
      case "core.textReplace": return wfShort(`${p.find || "…"} → ${p.replace || ""}`);
      case "core.textMatch": return wfShort(`/${p.pattern || ""}/`);
      case "core.math": return wfShort(`${String(p.a || "").replace(/\{\{\s*\$json\.?|\s*\}\}/g, "") || "a"} ${WF_SELECT_LABELS(spec, "operator", p.operator)}`);
      case "core.dateFormat": return p.format === "custom" ? wfShort(p.pattern) || "Custom pattern" : WF_SELECT_LABELS(spec, "format", p.format);
      case "core.dateAdjust": return p.operation === "startOf" || p.operation === "endOf" ? `${WF_SELECT_LABELS(spec, "operation", p.operation)} ${WF_SELECT_LABELS(spec, "unit", p.unit).toLowerCase()}` : `${WF_SELECT_LABELS(spec, "operation", p.operation)} ${p.amount || 1} ${WF_SELECT_LABELS(spec, "unit", p.unit).toLowerCase()}`;
      case "core.sort": return p.by ? `By ${p.by}, ${WF_SELECT_LABELS(spec, "order", p.order).toLowerCase()}` : WF_SELECT_LABELS(spec, "order", p.order);
      case "core.textCase": case "core.textSplit": case "core.textCombine": case "core.textEncode": case "core.hash":
      case "core.textExtract": case "core.count": case "core.round": case "core.formatNumber": case "core.random":
      case "core.statistics": case "core.combineItems": case "core.dedupe": case "core.pick": case "core.json":
      case "core.url": case "core.dateDiff": {
        const select = (spec.fields || []).find((field) => field.type === "select");
        const save = p.field || (spec.fields || []).find((field) => field.key === "field")?.default;
        const head = select ? WF_SELECT_LABELS(spec, select.key, p[select.key]) : spec.name;
        return wfShort(save ? `${head} → ${save}` : head);
      }
      case "action.trigger": {
        const events = wfChecks(p.events);
        return events.length ? `On ${events.map((e) => e === "workflow_dispatch" ? "manual run" : e.replace("_", " ")).join(", ")}` : "No events yet";
      }
      case "action.job": return wfList(p.runsOn).length ? `Runs on ${wfList(p.runsOn).join(", ")}` : "Steps run in order";
      case "action.step": return wfShort(wfFirstLine(p.run)) || "No command yet";
      case "shortcut.header": return [p.agent, p.model].filter(Boolean).join(" · ") || wfShort(p.description) || "Shortcut";
      case "skill.header": return wfShort(p.description) || "Skill";
      case "shortcut.run": {
        const file = wfShortcutLink(node);
        return file ? `Runs ${file}` : wfShort(wfStepCommand(p.text)) || "Empty";
      }
      case "shortcut.block": return wfShort(wfStepCommand(p.text)) || "Empty";
      case "skill.block": return wfShort(wfFirstLine(p.text)) || "Empty";
      case "agent.profile": return wfShort(p.specialty || p.focus) || "Special agent";
      case "agent.schedule": {
        const active = String(p.active) === "true" ? "" : " (off)";
        const days = wfChecks(p.days).map((d) => WF_DAYS[Number(d) - 1]).filter(Boolean).join(" ");
        const label = { "on-demand": "Only when asked", continuous: "Continuously", hourly: "Every hour", daily: `Daily at ${p.time || "09:00"}`, weekly: `Weekly at ${p.time || "09:00"}`, weekdays: `Weekdays at ${p.time || "09:00"}`, custom: `${days || "Chosen days"} at ${p.time || "09:00"}`, events: "On events" }[p.mode] || p.mode;
        return `${label}${active}`;
      }
      case "agent.events": return wfChecks(p.events).length ? wfChecks(p.events).join(", ") : "No events";
      case "agent.monitor": return `${p.badge || "Badge"} ≥ ${p.threshold || 0}`;
      case "agent.shortcut":
      case "agent.skill": return p.file || "Choose a file";
      case "agent.workflow": return p.workflow || "None";
      case "agent.relay": return p.channel || "#room";
      default: {
        const op = wfOperation(node);
        if (!op) return spec.description || "";
        const first = op.fields.find((f) => f.required && p[f.key] && !String(p[f.key]).includes("{{"));
        return wfShort(first ? `${op.name} · ${p[first.key]}` : op.name);
      }
    }
  }

  function wfSetNotice(text, tone = "good") {
    const view = wfView();
    view.notice = text;
    view.noticeTone = tone;
    const root = $("[data-wf-notice]");
    if (!root) return;
    root.hidden = !text;
    root.dataset.tone = tone;
    root.textContent = text;
  }

  function wfMarkDirty() {
    const view = wfView();
    view.dirty = true;
    wfRenderSaveButton();
  }

  function wfRenderSaveButton() {
    const view = wfView();
    const save = $("[data-wf-save]");
    if (!save) return;
    const label = wfIsFileMode() ? "Save to repository" : "Save";
    save.innerHTML = `<i data-lucide="save"></i><span class="wf-btn-label">${label}${view.dirty ? "*" : ""}</span>`;
    save.title = label;
    window.lucide?.createIcons();
  }

  function wfPortPoint(node, kind, output) {
    if (kind === "in") return { x: node.x, y: node.y + WF_NODE_H / 2 };
    const outputs = wfOutputs(node.type);
    const index = Math.max(0, outputs.indexOf(output));
    return { x: node.x + WF_NODE_W, y: node.y + (WF_NODE_H * (index + 1)) / (outputs.length + 1) };
  }

  function wfCurve(from, to) {
    const dx = Math.max(40, Math.abs(to.x - from.x) / 2);
    return `M ${from.x} ${from.y} C ${from.x + dx} ${from.y}, ${to.x - dx} ${to.y}, ${to.x} ${to.y}`;
  }

  function wfRunNode(id) {
    const view = wfView();
    if (view.live) return view.live.nodes[id] || null;
    return view.run?.nodes?.[id] || null;
  }

  function wfRunCount(entry, output) {
    if (!entry?.outputs) return null;
    if (output) return entry.outputs[output]?.count ?? null;
    return Object.values(entry.outputs).reduce((sum, item) => sum + (item.count || 0), 0);
  }

  function wfApplyTransform() {
    const view = wfView();
    const world = $("[data-wf-world]");
    const canvas = $("[data-wf-canvas]");
    if (world) world.style.transform = `translate(${view.panX}px, ${view.panY}px) scale(${view.zoom})`;
    if (canvas) {
      const size = 20 * view.zoom;
      canvas.style.backgroundSize = `${size}px ${size}px`;
      canvas.style.backgroundPosition = `${view.panX}px ${view.panY}px`;
    }
  }

  function wfRenderEdges(draft, hidden = -1) {
    const view = wfView();
    const svg = $("[data-wf-edges]");
    const current = view.current;
    if (!svg || !current) return;
    const parts = [];
    const handles = [];
    current.connections.forEach((conn, index) => {
      if (index === hidden) return;
      const from = wfNode(conn.from);
      const to = wfNode(conn.to);
      if (!from || !to) return;
      const a = wfPortPoint(from, "out", conn.output);
      const b = wfPortPoint(to, "in");
      const d = wfCurve(a, b);
      const selected = view.selectedEdge === index;
      parts.push(`<path class="wf-edge-hit" data-wf-edge="${index}" d="${d}"></path><path class="wf-edge${selected ? " is-selected" : ""}" d="${d}"></path>`);
      parts.push(`<path class="wf-edge-arrow" d="M ${b.x - 9} ${b.y - 5} L ${b.x - 2} ${b.y} L ${b.x - 9} ${b.y + 5} Z"></path>`);
      const count = wfRunCount(wfRunNode(conn.from), conn.output);
      if (count !== null) {
        const label = `${count} item${count === 1 ? "" : "s"}`;
        parts.push(`<text class="wf-edge-label" x="${(a.x + b.x) / 2}" y="${(a.y + b.y) / 2 - 6}" text-anchor="middle">${label}</text>`);
      }
      if (selected) {
        handles.push(`<circle class="wf-edge-handle" data-wf-edge-end="from" data-wf-edge-index="${index}" cx="${a.x}" cy="${a.y}" r="9"></circle>`);
        handles.push(`<circle class="wf-edge-handle" data-wf-edge-end="to" data-wf-edge-index="${index}" cx="${b.x}" cy="${b.y}" r="9"></circle>`);
      }
    });
    if (draft) parts.push(`<path class="wf-edge is-draft" d="${wfCurve(draft.from, draft.to)}"></path>`);
    svg.innerHTML = `<g transform="translate(${WF_SVG_OFFSET} ${WF_SVG_OFFSET})">${parts.join("")}${handles.join("")}</g>`;
  }

  function wfNodeHtml(node) {
    const view = wfView();
    const spec = wfSpec(node.type);
    const outputs = spec.outputs;
    const entry = wfIsFileMode() ? null : wfRunNode(node.id);
    const classes = ["wf-node"];
    if (spec.trigger) classes.push("is-trigger");
    if (view.selected === node.id) classes.push("is-selected");
    if (node.disabled) classes.push("is-disabled");
    if (outputs.length > 1) classes.push("has-branches");
    let status = "";
    if (entry) {
      const label = entry.status === "running"
        ? "Running"
        : entry.status === "success"
          ? `${wfRunCount(entry)} item${wfRunCount(entry) === 1 ? "" : "s"}`
          : entry.status === "error" ? "Error" : "Skipped";
      status = `<span class="wf-node-status" data-status="${escapeHtml(entry.status)}">${escapeHtml(label)}</span>`;
    }
    const ports = outputs.map((output, index) => {
      const top = (100 * (index + 1)) / (outputs.length + 1);
      const label = outputs.length > 1
        ? `<span class="wf-port-label" style="top:${top}%">${escapeHtml(WF_OUTPUT_LABELS[output] || output)}</span>`
        : "";
      return `${label}<span class="wf-port wf-port-out" data-wf-port-out="${escapeHtml(node.id)}" data-wf-output="${escapeHtml(output)}" style="top:calc(${top}% - var(--wf-port) / 2)" title="Drag to connect"></span>`;
    }).join("");
    const title = wfNodeTitle(node);
    const sub = node.disabled ? "Disabled" : wfNodeSummary(node);
    const nature = wfNature(node);
    const tag = WF_SCRIPT_TAGS[nature.kind] || "";
    const link = wfShortcutLink(node);
    const open = link
      ? `<button type="button" class="wf-node-link" data-wf-open-file="${escapeHtml(`.forkmesh/shortcuts/${link}`)}" aria-label="Open ${escapeHtml(link)}" title="Open ${escapeHtml(link)} on the canvas"><i data-lucide="arrow-up-right"></i><span>Open</span></button>`
      : "";
    return `<div class="${classes.join(" ")}" data-wf-node="${escapeHtml(node.id)}" data-status="${escapeHtml(entry?.status || "")}" style="left:${node.x}px;top:${node.y}px;--wf-node-color:${escapeHtml(spec.color || "#64748b")}" role="button" aria-label="${escapeHtml(`${title}${tag ? ` (${tag.toLowerCase()})` : ""}: ${sub}`)}">
      ${status}
      ${spec.inputs ? `<span class="wf-port wf-port-in" data-wf-port-in="${escapeHtml(node.id)}" title="Drag to move a connection"></span>` : ""}
      <div class="wf-node-row">
        ${wfBadge(node.type)}
        <div class="wf-node-text">
          <div class="wf-node-kicker"><span>${escapeHtml(spec.name)}</span>${tag ? `<span class="wf-node-tag" data-kind="${escapeHtml(nature.kind)}">${escapeHtml(tag)}</span>` : ""}</div>
          <div class="wf-node-title">${escapeHtml(title)}</div>
          ${open ? `<div class="wf-node-subline"><div class="wf-node-sub">${escapeHtml(sub)}</div>${open}</div>` : `<div class="wf-node-sub">${escapeHtml(sub)}</div>`}
        </div>
      </div>
      ${ports}
      ${outputs.length ? `<button type="button" class="wf-add-after" data-wf-add-after="${escapeHtml(node.id)}" aria-label="Add a step after ${escapeHtml(title)}" title="Add a step after this one"><i data-lucide="plus"></i></button>` : ""}
    </div>`;
  }

  function wfRenderCanvas() {
    const view = wfView();
    const root = $("[data-wf-nodes]");
    const svg = $("[data-wf-edges]");
    if (!root || !svg) return;
    svg.style.left = `${-WF_SVG_OFFSET}px`;
    svg.style.top = `${-WF_SVG_OFFSET}px`;
    svg.setAttribute("width", String(WF_SVG_OFFSET * 2));
    svg.setAttribute("height", String(WF_SVG_OFFSET * 2));
    const nodes = view.current?.nodes || [];
    root.innerHTML = nodes.map(wfNodeHtml).join("");
    const empty = $("[data-wf-empty]");
    if (empty) empty.hidden = Boolean(nodes.length) || !view.current || wfIsFileMode();
    wfRenderEdges();
    wfApplyTransform();
    window.lucide?.createIcons();
  }

  function wfRenderList() {
    const view = wfView();
    const root = $("[data-wf-list]");
    if (!root) return;
    if (view.loading) return;
    if (!view.workflows.length) {
      root.innerHTML = '<p class="wf-help px-2">No saved workflows yet. Build one on the canvas and press Save, or start from an example.</p>';
      return;
    }
    root.innerHTML = view.workflows.map((item) => {
      const current = !wfIsFileMode() && view.current?.id === item.id ? "true" : "false";
      const status = item.active ? "active" : (item.lastStatus || "");
      const meta = [
        item.active ? "Active" : "Inactive",
        `${item.nodes} step${item.nodes === 1 ? "" : "s"}`,
        item.lastRunAt ? `ran ${formatTimeAgo(item.lastRunAt)}` : "",
      ].filter(Boolean).join(" · ");
      return `<button type="button" class="wf-list-item" data-wf-open="${escapeHtml(item.id)}" aria-current="${current}">
        <span class="font-semibold truncate">${escapeHtml(item.name)}</span>
        <span class="wf-list-meta"><span class="wf-dot" data-status="${escapeHtml(status)}"></span>${escapeHtml(meta)}</span>
      </button>`;
    }).join("");
  }

  function wfRenderFiles() {
    const view = wfView();
    const root = $("[data-wf-files]");
    if (!root) return;
    const branch = $("[data-wf-repo-branch]");
    const repoInput = $("[data-wf-repo-input]");
    if (repoInput && document.activeElement !== repoInput && view.repo) repoInput.value = `${view.repo.owner}/${view.repo.name}`;
    const options = $("[data-wf-repo-options]");
    if (options) options.innerHTML = view.repoOptions.map((name) => `<option value="${escapeHtml(name)}"></option>`).join("");
    if (view.repoLoading) {
      root.innerHTML = '<p class="wf-help px-2"><span class="fm-spinner" aria-hidden="true"></span>Reading the repository...</p>';
      return;
    }
    if (!view.repo || !view.repoFiles) {
      if (branch) branch.textContent = "";
      root.innerHTML = '<p class="wf-help px-2">Open a repository to edit its actions, shortcuts, skills and special agents.</p>';
      return;
    }
    if (branch) branch.textContent = `Branch ${view.repo.branch}. Saving commits to it.`;
    const kinds = view.catalog?.files?.kinds || {};
    const shipped = view.packageFiles || [];
    const fromPlugins = shipped.length ? `<div class="wf-list-group"><span>From plug-ins</span></div>${shipped.map((item, index) => {
      const present = (view.repoFiles[item.kind] || []).includes(item.path);
      return `<button type="button" class="wf-file" data-wf-add-package-file="${index}" title="${escapeHtml(`${item.path} from ${item.packageName}`)}"><i data-lucide="${present ? "check" : "plus"}"></i><span>${escapeHtml(item.name)}${present ? " (in repository)" : ""}</span></button>`;
    }).join("")}` : "";
    root.innerHTML = fromPlugins + Object.entries(kinds).map(([kind, meta]) => {
      const files = view.repoFiles[kind] || [];
      const items = files.length
        ? files.map((path) => {
          const current = wfIsFileMode() && view.current?.path === path ? "true" : "false";
          return `<button type="button" class="wf-file" data-wf-open-file="${escapeHtml(path)}" aria-current="${current}" title="${escapeHtml(path)}"><i data-lucide="${escapeHtml(meta.icon)}"></i><span>${escapeHtml(path.split("/").pop())}</span></button>`;
        }).join("")
        : `<p class="wf-help px-2 mt-0">None yet.</p>`;
      return `<div class="wf-list-group"><span>${escapeHtml(meta.plural)}</span><button type="button" class="wf-icon-btn" data-wf-new-file="${escapeHtml(kind)}" aria-label="New ${escapeHtml(meta.label.toLowerCase())}" title="New ${escapeHtml(meta.label.toLowerCase())}"><i data-lucide="plus"></i></button></div>${items}`;
    }).join("");
    window.lucide?.createIcons();
  }

  function wfRenderSide() {
    const view = wfView();
    for (const tab of $$("[data-wf-mode]")) tab.setAttribute("aria-selected", tab.dataset.wfMode === view.mode ? "true" : "false");
    const autoSide = $("[data-wf-side-automations]");
    const fileSide = $("[data-wf-side-files]");
    const filesOn = wfPluginOn(WF_FILES_PACKAGE);
    if (!filesOn && view.mode === "files") view.mode = "automations";
    const tabs = $("[data-wf-app] .wf-tabs");
    if (tabs) tabs.hidden = !filesOn;
    for (const button of $$("[data-wf-open-templates]")) button.hidden = !wfPluginOn(WF_EXAMPLES_PACKAGE);
    if (autoSide) autoSide.hidden = view.mode === "files";
    if (fileSide) fileSide.hidden = view.mode !== "files";
    wfRenderList();
    wfRenderFiles();
  }

  function wfIsRoot(node) {
    const kind = wfView().current?.kind;
    const root = wfView().catalog?.files?.kinds?.[kind]?.root;
    return node.type === root;
  }

  function wfRenderToolbar() {
    const view = wfView();
    const current = view.current;
    const fileMode = wfIsFileMode();
    for (const el of $$("[data-wf-automation-only]")) el.hidden = fileMode;
    for (const el of $$("[data-wf-file-only]")) el.hidden = !fileMode;
    const name = $("[data-wf-name]");
    if (name) {
      name.readOnly = fileMode;
      if (document.activeElement !== name) {
        const root = fileMode ? current.nodes.find((n) => wfIsRoot(n)) : null;
        name.value = fileMode ? (root ? wfNodeTitle(root) : current.name) : (current?.name || "");
      }
    }
    const path = $("[data-wf-path]");
    if (path) {
      path.hidden = !fileMode;
      path.textContent = fileMode ? `${current.path}${current.isNew ? " (new)" : ""}` : "";
    }
    const active = $("[data-wf-active]");
    if (active) active.checked = Boolean(current?.active);
    const label = $("[data-wf-active-label]");
    if (label) label.textContent = current?.active ? "Active" : "Inactive";
    for (const button of $$("[data-wf-toolbar] button")) {
      if (button.matches("[data-wf-toggle-list], [data-wf-toggle-sheet]")) continue;
      button.disabled = !current || view.busy;
    }
    const del = $("[data-wf-delete]");
    if (del) del.disabled = !current?.id || view.busy;
    const toggle = $("[data-wf-runs-toggle]");
    if (toggle) {
      toggle.disabled = !current?.id;
      toggle.setAttribute("aria-expanded", view.runsOpen ? "true" : "false");
    }
    const menu = $("[data-wf-menu]");
    if (menu) menu.hidden = !view.menuOpen;
    $("[data-wf-menu-toggle]")?.setAttribute("aria-expanded", view.menuOpen ? "true" : "false");
    wfRenderSaveButton();
  }

  function wfInputItem(node) {
    const view = wfView();
    if (!view.run || !view.current || wfIsFileMode()) return null;
    for (const conn of view.current.connections) {
      if (conn.to !== node.id) continue;
      const items = wfRunNode(conn.from)?.outputs?.[conn.output]?.items;
      if (items?.length) return items[0];
    }
    return null;
  }

  function wfFieldValue(field, node) {
    const raw = node.params?.[field.key];
    if (raw === undefined || raw === null) return field.default ?? "";
    if (Array.isArray(raw) && field.type !== "checks") return raw.join("\n");
    if (typeof raw === "object" && !Array.isArray(raw)) return JSON.stringify(raw, null, 2);
    return raw;
  }

  function wfFieldHtml(field, node) {
    const value = wfFieldValue(field, node);
    const id = `wf-field-${node.id}-${field.key}`;
    const label = `<label class="wf-label" for="${escapeHtml(id)}">${escapeHtml(field.label)}${field.required ? " *" : ""}</label>`;
    const readonly = node.type === "agent.profile" && field.key === "id" ? " readonly" : "";
    const attrs = `id="${escapeHtml(id)}" data-wf-param="${escapeHtml(field.key)}" placeholder="${escapeHtml(field.placeholder || "")}"${readonly}`;
    if (field.type === "boolean") {
      const checked = ["true", "1", "yes", "on"].includes(String(value).toLowerCase()) ? " checked" : "";
      return `<label class="wf-check"><input type="checkbox" ${attrs}${checked} />${escapeHtml(field.label)}</label>`;
    }
    if (field.type === "checks") {
      const chosen = new Set(wfChecks(value));
      return `<p class="wf-label">${escapeHtml(field.label)}</p><div class="wf-checks" role="group" aria-label="${escapeHtml(field.label)}">${(field.options || []).map((option) => `<label class="wf-check"><input type="checkbox" data-wf-check="${escapeHtml(field.key)}" value="${escapeHtml(option.value)}"${chosen.has(String(option.value)) ? " checked" : ""} />${escapeHtml(option.label)}</label>`).join("")}</div>`;
    }
    if (field.type === "select") {
      const options = (field.options || []).map((option) => {
        const selected = String(option.value) === String(value) ? " selected" : "";
        return `<option value="${escapeHtml(option.value)}"${selected}>${escapeHtml(option.label)}</option>`;
      }).join("");
      return `${label}<select class="wf-select" ${attrs}>${options}</select>`;
    }
    if (field.type === "code") {
      return `${label}<textarea class="wf-textarea wf-code" ${attrs} spellcheck="false" wrap="off">${escapeHtml(value)}</textarea>`;
    }
    if (field.type === "text" || field.type === "json" || field.type === "list") {
      const mono = field.type === "json" ? " wf-mono" : "";
      return `${label}<textarea class="wf-textarea${mono}" ${attrs} spellcheck="false">${escapeHtml(value)}</textarea>`;
    }
    const mode = field.type === "number" ? ' inputmode="numeric"' : "";
    return `${label}<input class="wf-input" type="text" ${attrs}${mode} value="${escapeHtml(value)}" spellcheck="false" />`;
  }

  function wfOutputHtml(node) {
    const entry = wfRunNode(node.id);
    if (!entry) return '<p class="wf-help">Run the workflow to see what this step receives and returns.</p>';
    const parts = [`<div class="wf-row"><span class="wf-dot" data-status="${escapeHtml(entry.status)}"></span><span class="text-sm font-semibold">${escapeHtml(entry.status === "success" ? "Succeeded" : entry.status === "error" ? "Failed" : "Skipped")}</span>${entry.ms !== undefined ? `<span class="wf-help mt-0">${escapeHtml(String(entry.ms))} ms</span>` : ""}</div>`];
    if (entry.error) parts.push(`<div class="wf-pre" style="color:#dc2626">${escapeHtml(entry.error)}</div>`);
    if (entry.status === "error" && wfFailedRun()) parts.push(`<div class="wf-row mt-3">${wfFailureButton()}</div>`);
    if (entry.logs?.length) {
      parts.push(`<p class="wf-help">Script log, ${entry.logs.length} line${entry.logs.length === 1 ? "" : "s"}</p>`);
      parts.push(`<pre class="wf-pre">${escapeHtml(entry.logs.join("\n"))}</pre>`);
    }
    for (const [name, output] of Object.entries(entry.outputs || {})) {
      const title = Object.keys(entry.outputs).length > 1 ? `${name}: ` : "";
      parts.push(`<p class="wf-help">${escapeHtml(title)}${output.count} item${output.count === 1 ? "" : "s"}${output.count > output.items.length ? `, first ${output.items.length} shown` : ""}</p>`);
      if (output.items.length) parts.push(`<pre class="wf-pre">${escapeHtml(JSON.stringify(output.items, null, 2))}</pre>`);
    }
    return parts.join("");
  }

  function wfFilePath() {
    return String(wfView().current?.path || "this file");
  }

  function wfShortcutIsScript() {
    const name = wfFilePath().split("/").pop().toLowerCase();
    return WF_SCRIPT_EXTENSIONS.some((ext) => name.endsWith(ext)) || !name.includes(".");
  }

  function wfNature(node) {
    const spec = wfSpec(node.type);
    const path = wfFilePath();
    const script = wfCore(node.type)?.script;
    if (script) {
      return { kind: "script-part", label: "Part of a script", icon: "file-code", text: `One part of ${script.path} (${script.language}). Your desktop runs only this part of the script, with --step ${script.step}.` };
    }
    if (node.type === "core.code") {
      return { kind: "script", label: "Script", icon: "file-code", text: `Your own JavaScript. ${platformName()}'s sandboxed script interpreter runs it on the server, or on the phone that runs the workflow; it cannot reach files, the network or the host.` };
    }
    if (node.type === "core.comment") return { kind: "note", label: "Note", icon: "sticky-note", text: "A note on the canvas. It does not run and passes nothing on." };
    if (wfIsFileMode()) {
      if (wfIsRoot(node)) return { kind: "file-part", label: "File header", icon: "file-text", text: `The start of ${path}: its name and the settings that apply to the whole file.` };
      if (node.type === "action.step") return { kind: "script-part", label: "Part of a script", icon: "file-code", text: `Shell commands: one step of the action ${path}. They run after the steps before them, in the job they are connected to.` };
      if (node.type === "shortcut.block" || node.type === "shortcut.run") {
        return wfShortcutIsScript()
          ? { kind: "script-part", label: "Part of a script", icon: "file-code", text: `One block of the shell script ${path}. The blocks are joined top to bottom and run as one script.` }
          : { kind: "file-part", label: "Part of a prompt", icon: "message-square-text", text: `One section of the prompt ${path}. The sections are joined and sent to the agent as one prompt.` };
      }
      if (node.type === "skill.block") return { kind: "file-part", label: "Part of a skill", icon: "book-open", text: `One section of the instructions in ${path}.` };
      if (node.type.startsWith("agent.")) return { kind: "file-part", label: "Agent setting", icon: "bot", text: `One setting of the special agent in ${path}.` };
      return { kind: "file-part", label: "Part of a file", icon: "file-text", text: `Part of ${path}.` };
    }
    if (spec.trigger) return { kind: "trigger", label: "Trigger", icon: "zap", text: "Starts the workflow. Every run begins here, and its item is the first data the next steps receive." };
    if (spec.kind === "app") return { kind: "integration", label: "Integration call", icon: "plug", text: `Calls the ${spec.name} API once for each item it receives, signed in with the credential you pick.` };
    if (node.type === "core.http") return { kind: "request", label: "Web request", icon: "globe", text: "Calls the address you enter once for each item it receives and passes the reply on." };
    if (node.type === "core.runWorkflow") return { kind: "workflow", label: "Runs another workflow", icon: "workflow", text: "Runs one of your other workflows with each item and passes on what it returns." };
    if (spec.kind === "core") return { kind: "built-in", label: "Built-in step", icon: "box", text: `A step built into ${platformName()}. It works on the items it receives and calls nothing outside ${platformName()}.` };
    return { kind: "unknown", label: "Unknown step", icon: "circle-help", text: `This step type is not in the catalog any more, so ${platformName()} cannot run it.` };
  }

  function wfRunsOn(node) {
    if (wfIsDesktopType(node.type)) return `Your desktop, through the ${desktopBrand()}desktop app`;
    if (node.type === "core.comment") return "Nowhere, it is a note";
    if (wfIsFileMode()) {
      const kind = wfView().current?.kind;
      if (kind === "action") return "Wherever the action's job runs";
      if (kind === "shortcut") return "Your desktop, when the shortcut is started";
      if (kind === "skill") return "In the agent that loads the skill";
      return "In the special agent";
    }
    return `The ${platformName()} server, or the phone that runs the workflow`;
  }

  function wfNeighbours(node) {
    const conns = wfView().current?.connections || [];
    const pick = (list, key) => list.map((conn) => ({ node: wfNode(conn[key]), output: conn.output })).filter((item) => item.node);
    return {
      from: pick(conns.filter((conn) => conn.to === node.id), "from"),
      to: pick(conns.filter((conn) => conn.from === node.id), "to"),
    };
  }

  function wfStepLinks(list, empty) {
    if (!list.length) return `<span class="wf-help mt-0">${escapeHtml(empty)}</span>`;
    return list.map(({ node, output }) => {
      const branch = output && output !== "main" ? ` <span class="wf-help mt-0">(${escapeHtml(WF_OUTPUT_LABELS[output] || output)})</span>` : "";
      return `<button type="button" class="wf-step-link" data-wf-select-node="${escapeHtml(node.id)}" title="Show this step">${wfBadge(node.type, true)}<span>${escapeHtml(wfNodeTitle(node))}</span>${branch}</button>`;
    }).join("");
  }

  function wfFact(label, html) {
    return `<dt>${escapeHtml(label)}</dt><dd>${html}</dd>`;
  }

  function wfCode(text) {
    return `<code class="wf-mono">${escapeHtml(text)}</code>`;
  }

  function wfCodeHtml(text, first = 1, marked = null) {
    const lines = String(text ?? "").replace(/\n$/, "").split("\n");
    const rows = lines.map((line, index) => {
      const number = first + index;
      const mark = marked && marked(number) ? " is-marked" : "";
      return `<span class="wf-code-line${mark}" data-wf-line="${number}"><span class="wf-code-no" aria-hidden="true">${number}</span>${escapeHtml(line) || " "}</span>`;
    });
    return `<pre class="wf-code-view">${rows.join("")}</pre>`;
  }

  function wfFillTemplate(value, params, keys, depth = 0) {
    if (depth > 6) return value;
    if (typeof value === "string") {
      return value.replace(/\{([A-Za-z0-9_]+)\}/g, (all, key) => {
        const filled = keys.has(key) ? params[key] : undefined;
        return filled === undefined || filled === null || filled === "" ? all : String(filled);
      });
    }
    if (Array.isArray(value)) return value.map((item) => wfFillTemplate(item, params, keys, depth + 1));
    if (value && typeof value === "object") {
      return Object.fromEntries(Object.entries(value).map(([key, item]) => [key, wfFillTemplate(item, params, keys, depth + 1)]));
    }
    return value;
  }

  function wfObject(value) {
    if (value && typeof value === "object" && !Array.isArray(value)) return value;
    try {
      const parsed = JSON.parse(String(value));
      return parsed && typeof parsed === "object" && !Array.isArray(parsed) ? parsed : null;
    } catch {
      return null;
    }
  }

  function wfPairs(map) {
    return Object.entries(map || {}).map(([key, value]) => `${key}: ${value}`).join("\n");
  }

  function wfRequestHtml(node, app) {
    const op = wfOperation(node);
    const spec = wfView().catalog?.requests?.[app.key];
    const sent = spec?.operations?.[op?.key];
    if (!op || !sent) return "";
    const params = node.params || {};
    const keys = new Set((op.fields || []).map((field) => field.key));
    const secrets = new Set((app.credential || []).filter((field) => field.secret).map((field) => field.label));
    const address = `${spec.base || ""}${sent.path || ""}`;
    const filled = wfFillTemplate(address, Object.fromEntries((op.fields || []).map((field) => [field.key, params[field.key] ?? field.default])), keys);
    const cred = wfView().credentials.find((item) => item.id === node.credential);
    const auth = spec.auth || {};
    const sends = [];
    if (auth.headers) sends.push(`header ${wfPairs(auth.headers)}`);
    if (auth.query) sends.push(`query ${Object.entries(auth.query).map(([key, value]) => `${key}=${value}`).join("&")}`);
    if (auth.basic) sends.push(`HTTP Basic sign-in as ${auth.basic[0]}`);
    if (auth.body) sends.push(`body ${Object.keys(auth.body).join(", ")}`);
    if (!sends.length && (spec.base || "").startsWith("{")) sends.push("the whole address is the credential");
    const main = [
      wfFact("Request", `${wfCode(`${op.method} ${filled}`)}${filled !== address ? `<span class="wf-help mt-0">Template ${escapeHtml(address)}</span>` : ""}`),
      wfFact("Credential", app.credential?.length ? `${escapeHtml(cred ? cred.name : "None chosen yet")}<span class="wf-help mt-0">${escapeHtml(sends.join("; ") || "Sent by the integration")}${secrets.size ? `. ${escapeHtml(Array.from(secrets).join(", "))} ${secrets.size === 1 ? "stays in the credential and never shows" : "stay in the credential and never show"} here.` : ""}</span>` : "None needed"),
    ];
    const values = {};
    for (const field of op.fields || []) {
      const value = params[field.key] ?? field.default;
      if (value !== undefined && value !== null && value !== "") values[field.key] = value;
    }
    const fill = (value) => wfFillTemplate(value, values, keys);
    const inPath = new Set(Array.from(String(sent.path || "").matchAll(/\{([A-Za-z0-9_]+)\}/g), (match) => match[1]));
    const headers = { ...(spec.headers || {}), ...fill(sent.headers || {}) };
    const query = { ...fill(sent.query || {}) };
    const bodyFields = {};
    for (const field of op.fields || []) {
      const value = values[field.key];
      if (inPath.has(field.key) || value === undefined || field.in === "template") continue;
      const where = field.in || (WF_BODY_METHODS.includes(op.method) ? "body" : "query");
      if (where === "query") query[field.key.split(".").pop()] = value;
      else if (where === "query_object") Object.assign(query, wfObject(value) || { [field.key]: value });
      else if (where === "header") headers[field.header || field.key] = value;
      else bodyFields[field.key] = value;
    }
    let body = sent.body !== undefined ? fill(sent.body) : null;
    if (Object.keys(bodyFields).length && (body === null || (typeof body === "object" && !Array.isArray(body)))) {
      body = body || {};
      const used = JSON.stringify(sent.body ?? "");
      for (const [key, value] of Object.entries(bodyFields)) {
        if (used.includes(`{${key}}`)) continue;
        const path = key.split(".");
        let target = body;
        for (const part of path.slice(0, -1)) target = target[part] = target[part] && typeof target[part] === "object" ? target[part] : {};
        target[path[path.length - 1]] = value;
      }
    }
    const facts = [];
    if (Object.keys(headers).length) facts.push(wfFact("Headers", `<pre class="wf-pre mt-0">${escapeHtml(wfPairs(headers))}</pre>`));
    if (Object.keys(query).length) facts.push(wfFact("Query", `<pre class="wf-pre mt-0">${escapeHtml(wfPairs(query))}</pre>`));
    if (body !== null && WF_BODY_METHODS.includes(op.method)) facts.push(wfFact((sent.encoding || spec.encoding) === "form" ? "Form body" : "Body", `<pre class="wf-pre mt-0">${escapeHtml(JSON.stringify(body, null, 2))}</pre>`));
    const more = facts.length ? `<details class="wf-more"><summary>What it sends</summary><dl class="wf-facts">${facts.join("")}</dl><p class="wf-help">Values in braces come from this step's settings; <code>{{ ... }}</code> is filled in from each item when it runs.</p></details>` : "";
    return `<dl class="wf-facts">${main.join("")}</dl>${more}`;
  }

  function wfSettingText(field, node) {
    const value = wfFieldValue(field, node);
    if (field.type === "boolean") return ["true", "1", "yes", "on"].includes(String(value).toLowerCase()) ? "Yes" : "No";
    if (field.type === "checks") {
      const chosen = new Set(wfChecks(value));
      const labels = (field.options || []).filter((option) => chosen.has(String(option.value))).map((option) => option.label);
      return labels.join(", ");
    }
    if (field.type === "select") return (field.options || []).find((option) => String(option.value) === String(value))?.label || String(value ?? "");
    if (field.type === "code") {
      const count = String(value || "").split("\n").length;
      return value ? `${count} line${count === 1 ? "" : "s"} of code` : "";
    }
    return wfShort(value, 120);
  }

  function wfSettingsHtml(node) {
    const fields = wfFields(node);
    const rows = fields.map((field) => {
      const text = wfSettingText(field, node);
      return wfFact(field.label, text ? escapeHtml(text) : '<span class="wf-help mt-0">Not set</span>');
    });
    const saved = JSON.stringify(node.params || {}, null, 2);
    return `<details class="wf-more"><summary>Settings${fields.length ? ` (${fields.length})` : ""}</summary>${rows.length ? `<dl class="wf-facts">${rows.join("")}</dl>` : '<p class="wf-help">This step has no settings.</p>'}<p class="wf-label">Saved as</p><pre class="wf-pre mt-0">${escapeHtml(saved)}</pre></details>`;
  }

  function wfBlockText(node) {
    return String(node.type === "action.step" ? node.params?.run || "" : node.params?.text || "");
  }

  function wfBlockPlace(node) {
    const current = wfView().current;
    const family = node.type.startsWith("shortcut.") ? ["shortcut.block", "shortcut.run"] : [node.type];
    const blocks = wfRunOrder(current).filter((item) => family.includes(item.type));
    const index = blocks.findIndex((item) => item.id === node.id);
    return index < 0 ? "" : `${node.type === "action.step" ? "Step" : "Block"} ${index + 1} of ${blocks.length}`;
  }

  function wfScriptHtml(node, nature) {
    const view = wfView();
    const info = wfCore(node.type)?.script;
    if (info) {
      const parts = (info.functions || []).map((name) => `<button type="button" class="wf-chip" data-wf-view-script="${escapeHtml(name)}" title="Show ${escapeHtml(name)}() in the script">${escapeHtml(name)}()</button>`).join("");
      return `<div class="wf-script" data-wf-script-area>
        <div class="wf-script-head"><i data-lucide="file-code"></i><span class="wf-mono">${escapeHtml(info.path)}</span><span class="wf-help mt-0">${escapeHtml(info.language)}</span></div>
        <dl class="wf-facts">
          ${wfFact("This part", wfCode(`--step ${info.step}`))}
          ${wfFact("Desktop runs", wfCode(info.command))}
          ${wfFact("Dry run", `${wfCode(info.dryRun)}<span class="wf-help mt-0">Lists what it would delete and deletes nothing.</span>`)}
          ${wfFact("Started by", wfCode(info.runner))}
        </dl>
        <p class="wf-label">Code this part runs</p><div>${parts}</div>
        <div class="wf-row mt-3"><button type="button" class="wf-btn" data-wf-view-script><i data-lucide="file-code"></i>View script</button><span class="wf-help mt-0">See ${escapeHtml(info.docs)}</span></div>
      </div>`;
    }
    if (node.type === "core.code") {
      const code = String(wfFieldValue(wfFields(node).find((field) => field.key === "code") || { key: "code" }, node) || "");
      const lines = code ? code.split("\n").length : 0;
      const mode = WF_SELECT_LABELS(wfSpec(node.type), "mode", node.params?.mode);
      const logs = wfRunNode(node.id)?.logs?.length || 0;
      return `<div class="wf-script" data-wf-script-area>
        <div class="wf-script-head"><i data-lucide="file-code"></i><span>${escapeHtml(node.name)}</span><span class="wf-help mt-0">JavaScript</span></div>
        <dl class="wf-facts">
          ${wfFact("Size", `${lines} line${lines === 1 ? "" : "s"}`)}
          ${wfFact("Runs", escapeHtml(mode))}
          ${wfFact("Last run log", logs ? `${logs} line${logs === 1 ? "" : "s"}` : '<span class="wf-help mt-0">None yet</span>')}
        </dl>
        <div class="wf-row mt-3"><button type="button" class="wf-btn" data-wf-view-script><i data-lucide="file-code"></i>View script</button><span class="wf-help mt-0">Edit it in Script below.</span></div>
      </div>`;
    }
    if (wfIsFileMode() && !wfIsRoot(node) && (nature.kind === "script-part" || ["shortcut.block", "skill.block"].includes(node.type))) {
      const text = wfBlockText(node);
      const place = wfBlockPlace(node);
      return `<div class="wf-script" data-wf-script-area>
        <div class="wf-script-head"><i data-lucide="${escapeHtml(nature.icon)}"></i><span class="wf-mono">${escapeHtml(view.current.path)}</span>${place ? `<span class="wf-help mt-0">${escapeHtml(place)}</span>` : ""}</div>
        ${text.trim() ? wfCodeHtml(text) : '<p class="wf-help">This part is empty.</p>'}
        <div class="wf-row mt-3"><button type="button" class="wf-btn" data-wf-view-script><i data-lucide="file-code"></i>View in the whole file</button></div>
      </div>`;
    }
    return "";
  }

  function wfAboutHtml(node) {
    const spec = wfSpec(node.type);
    const nature = wfNature(node);
    const near = wfNeighbours(node);
    const facts = [
      wfFact("Step type", `${wfCode(node.type)}${spec.group ? `<span class="wf-help mt-0">${escapeHtml(spec.name)} in ${escapeHtml(spec.group)}</span>` : ""}`),
      wfFact("Runs on", escapeHtml(wfRunsOn(node))),
    ];
    if (spec.inputs) facts.push(wfFact("Receives from", wfStepLinks(near.from, "Nothing is connected yet")));
    if (spec.outputs.length) facts.push(wfFact("Sends to", wfStepLinks(near.to, "Nothing yet, so this is a last step")));
    const flags = [node.disabled ? "Disabled, passes items through" : "", node.continueOnFail ? "The workflow goes on if it fails" : ""].filter(Boolean);
    if (flags.length) facts.push(wfFact("Flags", escapeHtml(flags.join("; "))));
    if (node.type === "core.http") facts.push(wfFact("Request", wfCode(`${node.params?.method || "GET"} ${node.params?.url || "(no address yet)"}`)));
    if (node.type === "core.runWorkflow") {
      const wanted = String(node.params?.workflow || "").trim();
      const target = wfView().workflows.find((item) => item.id === wanted || item.name === wanted);
      facts.push(wfFact("Workflow", target ? `<button type="button" class="wf-step-link" data-wf-open="${escapeHtml(target.id)}" title="Open this workflow">${escapeHtml(target.name)}</button>` : `<span class="wf-help mt-0">${escapeHtml(wanted ? `No saved workflow is called ${wanted}` : "Not chosen yet")}</span>`));
    }
    if (node.type === "shortcut.run") {
      const file = wfShortcutLink(node);
      if (file) facts.push(wfFact("Runs", `<button type="button" class="wf-step-link" data-wf-open-file="${escapeHtml(`.forkmesh/shortcuts/${file}`)}" title="Open this shortcut on the canvas">${escapeHtml(file)}</button>`));
    }
    const parts = [`<div class="wf-section wf-about" data-wf-about>
      <div class="wf-row"><span class="wf-kind" data-kind="${escapeHtml(nature.kind)}"><i data-lucide="${escapeHtml(nature.icon)}"></i>${escapeHtml(nature.label)}</span></div>
      <p class="wf-help">${escapeHtml(nature.text)}</p>
      <dl class="wf-facts">${facts.join("")}</dl>
      ${wfScriptHtml(node, nature)}
      ${spec.app ? wfRequestHtml(node, spec.app) : ""}
      ${wfSettingsHtml(node)}
    </div>`];
    return parts.join("");
  }

  async function wfDesktopScript() {
    const view = wfView();
    if (view.desktopScript) return view.desktopScript;
    const data = await fetchJson(WF_DESKTOP_SCRIPT);
    if (!data || typeof data.source !== "string") throw Object.assign(new Error("script_unavailable"), { code: "script_unavailable" });
    view.desktopScript = data;
    return data;
  }

  function wfScrollToMark(selector) {
    window.setTimeout(() => {
      const target = $("[data-wf-dialog-body]")?.querySelector(selector);
      target?.scrollIntoView?.({ block: "center" });
    }, 30);
  }

  async function wfOpenDesktopScript(node, info, focus) {
    const facts = `<dl class="wf-facts">
      ${wfFact("File", wfCode(info.path))}
      ${wfFact("This step", `${escapeHtml(node.name)}, ${wfCode(`--step ${info.step}`)}`)}
      ${wfFact("Desktop runs", wfCode(info.command))}
      ${wfFact("Dry run", wfCode(info.dryRun))}
      ${wfFact("Started by", wfCode(info.runner))}
    </dl>`;
    let script = null;
    let failure = "";
    try {
      script = await wfDesktopScript();
    } catch (error) {
      failure = wfErrorText(error, "The script's source could not be loaded.");
    }
    if (!script) {
      wfOpenDialog(`${info.path} · ${node.name}`, "file-code", `<div class="p-2">${facts}<p class="wf-help">${escapeHtml(failure)}</p></div>`, true);
      return;
    }
    const lines = script.source.replace(/\n$/, "").split("\n");
    const names = (script.steps?.[info.step] || info.functions || []).filter((name) => Array.isArray(script.functions?.[name]));
    const open = names.includes(focus) ? focus : names[0];
    const ranges = names.map((name) => script.functions[name]);
    const parts = names.map((name) => {
      const [start, end] = script.functions[name];
      return `<details class="wf-more wf-script-part" data-wf-script-part="${escapeHtml(name)}"${name === open ? " open" : ""}><summary><span class="wf-mono">${escapeHtml(name)}()</span><span class="wf-help mt-0">lines ${start} to ${end}</span></summary>${wfCodeHtml(lines.slice(start - 1, end).join("\n"), start)}</details>`;
    }).join("");
    const whole = `<details class="wf-more wf-script-part" data-wf-script-part=""><summary><span>Whole file</span><span class="wf-help mt-0">${lines.length} lines; the code this step runs is marked</span></summary>${wfCodeHtml(lines.join("\n"), 1, (number) => ranges.some(([start, end]) => number >= start && number <= end))}</details>`;
    wfOpenDialog(`${info.path} · ${node.name}`, "file-code", `<div class="p-2">${facts}<p class="wf-label">Code this step runs, in the order it is used</p>${parts}${whole}<p class="wf-help">This is the copy on the ${escapeHtml(platformName())} server. Your desktop runs the copy in its own checkout, which matches once both are up to date.</p></div>`, true);
    if (focus) wfScrollToMark(`[data-wf-script-part="${CSS.escape(focus)}"]`);
  }

  function wfOpenCodeScript(node) {
    const code = String(wfFieldValue(wfFields(node).find((field) => field.key === "code") || { key: "code" }, node) || "");
    const logs = wfRunNode(node.id)?.logs || [];
    const mode = WF_SELECT_LABELS(wfSpec(node.type), "mode", node.params?.mode);
    const log = logs.length ? `<p class="wf-label">Log from the selected run</p><pre class="wf-pre">${escapeHtml(logs.join("\n"))}</pre>` : "";
    wfOpenDialog(`${node.name} · Run script`, "file-code", `<div class="p-2"><dl class="wf-facts">${wfFact("Language", "JavaScript, the subset in docs/app/workflow-scripts.md")}${wfFact("Runs", escapeHtml(mode))}${wfFact("Reads", `${wfCode("items")} and ${wfCode("$json")}; what it returns becomes the step's items`)}</dl>${wfCodeHtml(code)}${log}</div>`, true);
  }

  function wfFindBlock(lines, text) {
    const block = String(text || "").split("\n").map((line) => line.trim());
    while (block.length && !block[0]) block.shift();
    while (block.length && !block[block.length - 1]) block.pop();
    if (!block.length) return null;
    for (let i = 0; i < lines.length; i += 1) {
      if (lines[i].trim() !== block[0]) continue;
      if (block.every((line, j) => (lines[i + j] ?? "").trim() === line)) return [i + 1, i + block.length];
    }
    return null;
  }

  async function wfOpenFileScript(node) {
    let data;
    try {
      data = await wfRenderFileText();
    } catch (error) {
      wfSetNotice(wfErrorText(error, "The file could not be built."), "bad");
      return;
    }
    const path = wfView().current.path;
    const lines = String(data.text || "").replace(/\n$/, "").split("\n");
    const range = wfFindBlock(lines, wfBlockText(node));
    const where = range
      ? `Lines ${range[0]} to ${range[1]} of ${lines.length}, marked below`
      : "Not in the file yet: connect it to the steps that are saved";
    const place = wfBlockPlace(node);
    wfOpenDialog(path, "file-code", `<div class="p-2"><dl class="wf-facts">${wfFact("File", wfCode(path))}${wfFact("This step", `${escapeHtml(wfNodeTitle(node))}${place ? `, ${escapeHtml(place.toLowerCase())}` : ""}`)}${wfFact("Where", escapeHtml(where))}</dl>${wfCodeHtml(lines.join("\n"), 1, range ? (number) => number >= range[0] && number <= range[1] : null)}</div>`, true);
    if (range) wfScrollToMark(`[data-wf-line="${range[0]}"]`);
  }

  async function wfOpenScript(focus = "") {
    const node = wfNode(wfView().selected);
    if (!node) return;
    const info = wfCore(node.type)?.script;
    if (info) return wfOpenDesktopScript(node, info, focus);
    if (node.type === "core.code") return wfOpenCodeScript(node);
    if (wfIsFileMode()) return wfOpenFileScript(node);
  }

  function wfCredentialOptions(app, node) {
    const creds = wfView().credentials.filter((cred) => cred.integration === app.key);
    const options = [`<option value="">${creds.length ? "Choose a credential" : "No credentials yet"}</option>`]
      .concat(creds.map((cred) => `<option value="${escapeHtml(cred.id)}"${cred.id === node.credential ? " selected" : ""}>${escapeHtml(cred.name)}</option>`));
    return options.join("");
  }

  function wfNodePanel(node) {
    const view = wfView();
    const spec = wfSpec(node.type);
    const app = spec.app;
    const fileMode = wfIsFileMode();
    const parts = [];
    parts.push(`<div class="wf-row">${wfBadge(node.type)}<div class="min-w-0"><h2>${escapeHtml(wfNodeTitle(node))}</h2><p class="wf-help mt-0">${escapeHtml(spec.name)}${spec.group ? ` · ${escapeHtml(spec.group)}` : ""}</p></div></div>`);
    parts.push(`<p class="wf-help">${escapeHtml(spec.description || "")}</p>`);
    parts.push(wfAboutHtml(node));
    if (!fileMode) {
      parts.push(`<label class="wf-label" for="wf-node-name">Step name</label><input id="wf-node-name" class="wf-input" type="text" maxlength="60" data-wf-node-name value="${escapeHtml(node.name)}" />`);
    }
    if (app) {
      const ops = app.operations.map((op) => `<option value="${escapeHtml(op.key)}"${op.key === node.params?.operation ? " selected" : ""}>${escapeHtml(op.name)}</option>`).join("");
      parts.push(`<label class="wf-label" for="wf-node-op">Action</label><select id="wf-node-op" class="wf-select" data-wf-operation>${ops}</select>`);
      if (app.credential.length) {
        parts.push(`<label class="wf-label" for="wf-node-cred">Credential</label><div class="wf-row"><select id="wf-node-cred" class="wf-select" data-wf-credential>${wfCredentialOptions(app, node)}</select><button type="button" class="wf-btn" data-wf-new-credential="${escapeHtml(app.key)}"><i data-lucide="plus"></i>New</button></div>`);
        if (app.host) parts.push(`<p class="wf-help">Calls ${escapeHtml(app.host)} with this credential.</p>`);
      }
    }
    if (node.type === "core.webhook") {
      const current = view.current;
      if (current?.id && current.hookPath) {
        const url = `${location.origin}${current.hookPath}`;
        parts.push(`<label class="wf-label">Webhook URL</label><div class="wf-row"><input class="wf-input wf-mono" type="text" readonly value="${escapeHtml(url)}" data-wf-hook-url /><button type="button" class="wf-btn" data-wf-copy-hook aria-label="Copy webhook URL"><i data-lucide="copy"></i></button></div>`);
        parts.push(`<p class="wf-help">Send GET or POST requests here. The JSON body, query parameters and method arrive as <code>$json.body</code>, <code>$json.query</code> and <code>$json.method</code>. The URL only answers while the workflow is active.</p>`);
      } else {
        parts.push('<p class="wf-help">Save the workflow to get its secret webhook URL.</p>');
      }
    }
    if (node.type === "core.schedule") {
      parts.push('<p class="wf-help">Runs every so many minutes (5 to 10,080) while the workflow is active, checked every five minutes. Scheduled runs can make up to 6 requests each.</p>');
    }
    const fields = wfFields(node);
    if (fields.length) {
      parts.push(fields.map((field) => wfFieldHtml(field, node)).join(""));
      if (!fileMode) parts.push('<p class="wf-help">Use <code>{{ $json.field }}</code> for data from the previous step, <code>{{ $node["Step name"].json.field }}</code> for any earlier step, and <code>{{ $now }}</code> for the current time.</p>');
    }
    const input = wfInputItem(node);
    if (input && typeof input === "object") {
      const keys = Object.keys(input).slice(0, 30);
      if (keys.length) {
        parts.push(`<div class="wf-section"><p class="wf-label mt-0">Input fields from the last run</p><div>${keys.map((key) => `<button type="button" class="wf-chip" data-wf-insert="${escapeHtml(key)}" title="Insert into the last field you edited">${escapeHtml(key)}</button>`).join("")}</div></div>`);
      }
    }
    if (!fileMode) {
      parts.push(`<div class="wf-section">
        <label class="wf-check"><input type="checkbox" data-wf-node-flag="disabled"${node.disabled ? " checked" : ""} />Disable this step (pass items through)</label>
        <label class="wf-check"><input type="checkbox" data-wf-node-flag="continueOnFail"${node.continueOnFail ? " checked" : ""} />Continue the workflow if this step fails</label>
      </div>`);
      parts.push(`<div class="wf-section"><p class="wf-label mt-0">Last run</p>${wfOutputHtml(node)}</div>`);
    }
    if (!(fileMode && wfIsRoot(node))) {
      parts.push(`<div class="wf-section"><button type="button" class="wf-btn wf-btn-danger" data-wf-delete-node><i data-lucide="trash-2"></i>Delete step</button></div>`);
    }
    return parts.join("");
  }

  function wfFileOverview() {
    const current = wfView().current;
    const kind = wfView().catalog?.files?.kinds?.[current.kind] || {};
    const tips = {
      action: "Steps run from the trigger along the connections, job by job. A step that is not connected is left out of the file.",
      shortcut: "Blocks are joined in connection order into the script or prompt, separated by a blank line.",
      skill: "Instruction blocks are joined in connection order, separated by a blank line.",
      agent: "Triggers on the left start the agent; the playbook, skill files, native workflow and relay channel on the right are what it uses. Only steps connected to the agent are saved.",
      workflow: "The desktop runs these steps in connection order: special agents, shortcuts, desktop steps and Save output. Each step gets the output of the steps before it, and every run keeps a Markdown output file on the desktop.",
    };
    return `<h2>${escapeHtml(kind.label || "File")}: ${escapeHtml(current.path)}</h2>
      <p class="wf-help">${escapeHtml(kind.description || "")}</p>
      <p class="wf-help">${escapeHtml(tips[current.kind] || "")}</p>
      <div class="wf-section"><p class="wf-help">Saving writes ${escapeHtml(current.path)} on ${escapeHtml(wfView().repo?.branch || "the default branch")} as one commit. You need write access to the repository. Use View file to check the result first.</p></div>`;
  }

  function wfOverviewPanel() {
    const view = wfView();
    const current = view.current;
    if (!current) return '<p class="wf-help">Pick a workflow or start a new one.</p>';
    if (view.selectedEdge >= 0 && current.connections[view.selectedEdge]) {
      const conn = current.connections[view.selectedEdge];
      const from = wfNode(conn.from);
      const to = wfNode(conn.to);
      return `<h2>Connection</h2><p class="wf-help">${escapeHtml(from ? wfNodeTitle(from) : "")}${conn.output !== "main" ? ` (${escapeHtml(conn.output)})` : ""} → ${escapeHtml(to ? wfNodeTitle(to) : "")}</p><p class="wf-help">Drag either round handle to another step to move this connection, or drop it on empty canvas to remove it.</p><button type="button" class="wf-btn wf-btn-danger mt-3" data-wf-delete-edge><i data-lucide="unlink"></i>Remove connection</button>`;
    }
    if (wfIsFileMode()) return wfFileOverview();
    const trigger = current.nodes.find((node) => wfIsTrigger(node.type));
    const run = view.run;
    const parts = [`<h2>${escapeHtml(current.name || "Untitled workflow")}</h2>`];
    parts.push(`<p class="wf-help">${current.nodes.length} step${current.nodes.length === 1 ? "" : "s"}, ${current.connections.length} connection${current.connections.length === 1 ? "" : "s"}. ${trigger ? `Starts from ${escapeHtml(wfSpec(trigger.type).name.toLowerCase())}.` : "Add a trigger to start."}</p>`);
    if (run) {
      parts.push(`<div class="wf-section"><p class="wf-label mt-0">Selected execution</p><div class="wf-row"><span class="wf-dot" data-status="${escapeHtml(run.status)}"></span><span class="text-sm">${escapeHtml(run.status === "success" ? "Succeeded" : "Failed")} · ${escapeHtml(wfTriggerLabel(run.trigger))} · ${escapeHtml(String(run.requests || 0))} requests</span></div>${run.output?.text && run.status === "success" ? `<p class="wf-help">Output: ${escapeHtml(run.output.text)}</p>` : ""}${run.error ? `<div class="wf-pre" style="color:#dc2626">${escapeHtml(run.error)}</div>` : ""}${wfFailedRun() ? `<div class="wf-row mt-3">${wfFailureButton()}</div>` : ""}<p class="wf-help">Select a step to see its output.</p></div>`);
    }
    parts.push(`<div class="wf-section"><p class="wf-label mt-0">Building</p><ul class="wf-help" style="list-style:disc;padding-left:1rem">
      <li>Add steps with Add step, the + next to a step, or a double-click (long press on touch) on empty canvas.</li>
      <li>Drag from the dot on the right of a step to another step to connect them.</li>
      <li>Drag a step's left dot, or the handles of a selected connection, to move a connection.</li>
      <li>Drag the background to move around; scroll or pinch to zoom.</li>
      <li>Select a step or connection and press Delete to remove it. Ctrl or Cmd + S saves.</li>
    </ul></div>`);
    parts.push(`<div class="wf-section"><p class="wf-label mt-0">Expressions</p><p class="wf-help"><code>{{ $json.email }}</code> a field of the item arriving at a step<br><code>{{ $node["Slack"].json.ts }}</code> the output of an earlier step<br><code>{{ $trigger.body.id }}</code> the trigger data<br><code>{{ $now }}</code>, <code>{{ $today }}</code>, <code>{{ $workflow.name }}</code></p></div>`);
    const packs = view.catalog?.packs || [];
    if (view.plugins && packs.length) {
      parts.push(`<div class="wf-section"><p class="wf-label mt-0">Integration packs</p><p class="wf-help">Each group of integrations is its own package. Turning one off hides it from Add step on every device; workflows that use it keep running.</p>${packs.map((pack) => `<label class="wf-check"><input type="checkbox" data-wf-pack="${escapeHtml(pack.id)}"${wfPluginOn(pack.id) ? " checked" : ""} />${escapeHtml(pack.category)} <span class="wf-help mt-0">(${pack.integrations.length})</span></label>`).join("")}
        <label class="wf-check"><input type="checkbox" data-wf-pack="${WF_EXAMPLES_PACKAGE}"${wfPluginOn(WF_EXAMPLES_PACKAGE) ? " checked" : ""} />Examples</label>
        <label class="wf-check"><input type="checkbox" data-wf-pack="${WF_FILES_PACKAGE}"${wfPluginOn(WF_FILES_PACKAGE) ? " checked" : ""} />Repository files</label></div>`);
    }
    parts.push('<div class="wf-section"><p class="wf-help">Workflows download as n8n-compatible JSON, and n8n workflow files can be uploaded or imported from a link. Credentials and workflow data are stored encrypted. Steps only call public https:// addresses.</p></div>');
    return parts.join("");
  }

  function wfRenderPanel() {
    const view = wfView();
    const root = $("[data-wf-panel-body]");
    if (!root) return;
    const node = view.selected ? wfNode(view.selected) : null;
    root.innerHTML = node ? wfNodePanel(node) : wfOverviewPanel();
    window.lucide?.createIcons();
  }

  function wfRenderRuns() {
    const view = wfView();
    const root = $("[data-wf-runs]");
    if (!root) return;
    root.hidden = !view.runsOpen || wfIsFileMode();
    if (root.hidden) return;
    if (!view.runs.length) {
      root.innerHTML = '<p class="wf-help px-4 py-3">No executions yet. Run the workflow, call its webhook or turn on its schedule.</p>';
      return;
    }
    root.innerHTML = view.runs.map((run) => {
      const current = view.run?.id === run.id ? "true" : "false";
      const ms = Math.max(0, (run.finishedAt || 0) - (run.startedAt || 0));
      return `<button type="button" class="wf-run" data-wf-run-id="${escapeHtml(run.id)}" aria-current="${current}">
        <span class="wf-dot" data-status="${escapeHtml(run.status)}"></span>
        <span class="font-semibold">${escapeHtml(run.status === "success" ? "Succeeded" : "Failed")}</span>
        <span class="wf-help mt-0">${escapeHtml(wfTriggerLabel(run.trigger))} · ${escapeHtml(formatTimeAgo(run.startedAt))}</span>
        ${wfRunOutputHtml(run.output)}
        <span class="wf-help mt-0">${escapeHtml(wfDuration(ms))}</span>
      </button>`;
    }).join("");
  }

  function wfTriggerLabel(trigger) {
    return trigger === "desktop" ? "on your desktop" : String(trigger || "manual");
  }

  function wfRunOutputHtml(output) {
    if (!output || typeof output !== "object") return '<span class="wf-run-output is-empty" aria-hidden="true"></span>';
    const metrics = (Array.isArray(output.metrics) ? output.metrics : []).slice(0, 3);
    const parts = metrics.map((metric) => `<span class="wf-run-metric"><strong>${escapeHtml(metric.value)}</strong> ${escapeHtml(String(metric.label || "").toLowerCase())}</span>`);
    if (output.text && output.freedBytes === undefined) parts.push(`<span class="wf-run-text">${escapeHtml(output.text)}</span>`);
    if (!parts.length) return '<span class="wf-run-output is-empty" aria-hidden="true"></span>';
    return `<span class="wf-run-output" title="${escapeHtml(output.text || "")}">${parts.join("")}</span>`;
  }

  function wfFailedRun() {
    const view = wfView();
    if (view.live || wfIsFileMode()) return null;
    return view.run?.status === "error" ? view.run : null;
  }

  function wfFailureButton() {
    return '<button type="button" class="wf-btn" data-wf-send-failure title="Put this failure in the agent prompt so an agent can fix it"><i data-lucide="send"></i>Send to agent</button>';
  }

  function wfRedactParams(value, depth = 0) {
    if (depth > 6 || value === null || typeof value !== "object") return value;
    if (Array.isArray(value)) return value.map((item) => wfRedactParams(item, depth + 1));
    const secretPair = "value" in value && WF_SECRET_KEY_RE.test(String(value.name ?? value.key ?? ""));
    const out = {};
    for (const [key, item] of Object.entries(value)) {
      out[key] = WF_SECRET_KEY_RE.test(key) || (secretPair && key === "value") ? "[hidden]" : wfRedactParams(item, depth + 1);
    }
    return out;
  }

  function wfFailurePrompt(run) {
    const view = wfView();
    const workflow = view.current || { name: "", nodes: [], connections: [] };
    const nodes = run.nodes && typeof run.nodes === "object" ? run.nodes : {};
    const ordered = wfRunOrder(workflow).map((node) => node.id).filter((id) => nodes[id]);
    const ids = ordered.concat(Object.keys(nodes).filter((id) => !ordered.includes(id)));
    const where = `/dashboard/workflows${workflow.id ? `?id=${encodeURIComponent(workflow.id)}` : ""}`;
    const lines = [`The ${platformName()} workflow "${workflow.name || "Untitled workflow"}" failed (${where}). Find the cause and fix it.`, ""];
    const started = Number(run.startedAt) ? new Date(Number(run.startedAt)).toISOString() : "an unknown time";
    lines.push(`Run ${run.id || "(not saved)"}, ${wfTriggerLabel(run.trigger)}, started ${started}, failed after ${wfDuration((run.finishedAt || 0) - (run.startedAt || 0))}.`);
    if (run.error) lines.push(`Error: ${run.error}`);
    if (ids.length) lines.push("", "Steps:");
    for (const id of ids) {
      const node = wfNode(id);
      const entry = nodes[id] || {};
      const status = entry.status === "success" ? "succeeded" : entry.status === "error" ? "failed" : "skipped";
      const time = entry.status !== "skipped" && entry.ms !== undefined ? `, ${wfDuration(entry.ms)}` : "";
      lines.push(`- ${node ? `${node.name} (${node.type})` : id}: ${status}${time}`);
      if (entry.error) lines.push(`  Error: ${entry.error}`);
    }
    const failedId = ids.find((id) => nodes[id]?.status === "error");
    const failed = failedId ? wfNode(failedId) : null;
    if (failed) {
      const step = wfCore(failed.type)?.desktopStep;
      if (step) {
        lines.push("", `"${failed.name}" is a desktop step. The ${desktopBrand()}desktop app (MainWindow::runLocalControlFreeDiskSpace) runs \`python3 -u <checkout>/tools/free_disk_space.py --execute --step ${step} --repo <checkout> --json\` and reports the last line it printed as the error. \`python3 tools/free_disk_space.py --step ${step}\` repeats it as a dry run that deletes nothing. See docs/tools/free-disk-space.md and docs/app/workflows.md.`);
      } else {
        lines.push("", `Settings of "${failed.name}": ${wfShort(JSON.stringify(wfRedactParams(failed.params || {})), WF_FAILURE_PARAMS_MAX)}`, "See docs/app/workflows.md for how steps run.");
      }
      const logs = Array.isArray(nodes[failedId].logs) ? nodes[failedId].logs.map(String) : [];
      if (logs.length) {
        const tail = logs.slice(-WF_FAILURE_LOG_LINES);
        lines.push("", `Last ${tail.length} log line${tail.length === 1 ? "" : "s"} of "${failed.name}":`, "```", ...tail.map((line) => wfShort(line, 300)), "```");
      }
    }
    return lines.join("\n").slice(0, WF_FAILURE_PROMPT_MAX);
  }

  async function wfSendFailure() {
    const run = wfFailedRun();
    if (!run) return;
    const prompt = wfFailurePrompt(run);
    if (sendTextToPrompt(prompt)) {
      wfSetNotice("The failure is in the agent prompt. Pick the repository and agent, then send it.", "good");
      return;
    }
    try {
      await navigator.clipboard.writeText(prompt);
      wfSetNotice("There is no agent prompt on this page, so the failure report was copied instead.", "good");
    } catch {
      wfSetNotice("There is no agent prompt on this page and the failure report could not be copied.", "bad");
    }
  }

  function wfRailData() {
    const view = wfView();
    const current = view.current;
    if (!current || wfIsFileMode() || !wfHasDesktopSteps(current)) return null;
    const steps = wfRunOrder(current).filter((node) => !wfIsTrigger(node.type));
    const past = view.runs
      .filter((run) => (run.finishedAt || 0) > (run.startedAt || 0))
      .map((run) => ({ id: run.id, ms: run.finishedAt - run.startedAt, status: run.status, startedAt: run.startedAt }));
    const live = view.live;
    const now = Date.now();
    const marks = [];
    let elapsed = 0;
    let status = "";
    if (live) {
      elapsed = (live.finishedAt || now) - live.startedAt;
      status = live.finishedAt ? (Object.values(live.nodes).some((entry) => entry.status === "error") ? "error" : "success") : "running";
      for (const node of steps) {
        const entry = live.nodes[node.id];
        if (!entry || entry.status === "skipped") continue;
        if (entry.status === "running") marks.push({ node, status: "running", at: elapsed, ms: now - entry.startedAt });
        else marks.push({ node, status: entry.status, at: entry.finishedAt - live.startedAt, ms: entry.ms || 0 });
      }
    } else if (view.run?.nodes) {
      let at = 0;
      for (const node of steps) {
        const entry = view.run.nodes[node.id];
        if (!entry || entry.status === "skipped") continue;
        at += entry.ms || 0;
        marks.push({ node, status: entry.status, at, ms: entry.ms || 0 });
      }
      elapsed = Math.max(at, (view.run.finishedAt || 0) - (view.run.startedAt || 0));
      status = view.run.status;
    }
    const longest = past.reduce((max, run) => Math.max(max, run.ms), 0);
    return {
      steps, past, marks, elapsed, status, longest, live,
      selectedId: live ? "" : view.run?.id || "",
      scale: Math.max(longest, elapsed, 1),
    };
  }

  function wfRenderRail() {
    const root = $("[data-wf-rail]");
    if (!root) return;
    const data = wfRailData();
    root.hidden = !data;
    if (!data) return;
    const pct = (ms) => Math.min(100, Math.max(0, (ms / data.scale) * 100)).toFixed(2);
    const ticks = data.past.filter((run) => run.id !== data.selectedId).map((run) => {
      const label = wfDuration(run.ms);
      return `<span class="wf-rail-tick${run.ms === data.longest ? " is-longest" : ""}" data-status="${escapeHtml(run.status)}" style="left:${pct(run.ms)}%" title="${escapeHtml(`A run ${formatTimeAgo(run.startedAt)} took ${label}`)}"><span class="wf-rail-tick-label">${escapeHtml(label)}</span></span>`;
    }).join("");
    let previous = -100;
    let shift = 0;
    const marks = data.marks.map((mark, index) => {
      const at = Number(pct(mark.at));
      shift = at - previous < 3 ? shift + 1.1 : 0;
      previous = at;
      return `<span class="wf-rail-step" data-status="${escapeHtml(mark.status)}" style="left:calc(${at}% + ${shift.toFixed(1)}rem);z-index:${index + 3}" title="${escapeHtml(`${mark.node.name}: ${mark.status === "running" ? "running for " : ""}${wfDuration(mark.ms)}`)}">${wfBadge(mark.node.type, true)}</span>`;
    }).join("");
    const waiting = data.live
      ? data.steps.filter((node) => !data.live.nodes[node.id] && !data.live.finishedAt)
      : [];
    let headline = "Run it once to set the scale of the bar";
    if (data.live && !data.live.finishedAt) {
      const running = data.marks.find((mark) => mark.status === "running");
      headline = running ? `${running.node.name} · ${wfDuration(data.elapsed)}` : `Connecting to your desktop · ${wfDuration(data.elapsed)}`;
    } else if (data.live || data.marks.length) {
      headline = `${data.status === "error" ? "Failed after" : "Finished in"} ${wfDuration(data.elapsed)}`;
    } else if (data.past.length) {
      headline = `${data.past.length} run${data.past.length === 1 ? "" : "s"} so far`;
    }
    const scaleNote = data.longest ? `Full bar: longest run, ${wfDuration(data.longest)}` : "";
    const send = !data.live && data.status === "error" && wfFailedRun() ? wfFailureButton() : "";
    root.innerHTML = `<div class="wf-rail-head"><span class="wf-row"><span class="font-semibold">${escapeHtml(headline)}</span>${send}</span><span class="wf-help mt-0">${escapeHtml(scaleNote)}</span></div>
      <div class="wf-rail-track" role="progressbar" aria-label="Run progress" aria-valuemin="0" aria-valuemax="${Math.round(data.scale)}" aria-valuenow="${Math.round(data.elapsed)}" aria-valuetext="${escapeHtml(headline)}">
        <span class="wf-rail-fill" data-status="${escapeHtml(data.status)}" style="width:${pct(data.elapsed)}%"></span>
        ${ticks}${marks}
      </div>
      ${waiting.length ? `<div class="wf-rail-next"><span class="wf-help mt-0">Next</span>${waiting.map((node) => `<span title="${escapeHtml(node.name)}">${wfBadge(node.type, true)}</span>`).join("")}</div>` : ""}`;
    window.lucide?.createIcons();
  }

  function wfStopRailClock() {
    const view = wfView();
    if (view.railTimer) window.clearInterval(view.railTimer);
    view.railTimer = 0;
  }

  function wfStartRailClock() {
    const view = wfView();
    wfStopRailClock();
    view.railTimer = window.setInterval(() => {
      if (!wfView().live) {
        wfStopRailClock();
        return;
      }
      wfRenderRail();
    }, 250);
  }

  function wfRenderLayout() {
    const view = wfView();
    const app = $("[data-wf-app]");
    if (!app) return;
    app.classList.toggle("is-list-open", Boolean(view.listOpen));
    app.classList.toggle("is-sheet-open", Boolean(view.sheetOpen));
  }

  function renderWorkflowsPage() {
    const view = wfView();
    const app = $("[data-wf-app]");
    if (!app) return;
    if (view.pluginOff) {
      const main = $("[data-wf-main]");
      if (main) main.innerHTML = `<div class="wf-signed-out"><div><p class="text-sm font-semibold">Workflows is turned off for your account.</p><p class="wf-help">Install it to build automations and edit repository files. Your saved workflows are kept.</p><button type="button" class="wf-btn wf-btn-primary mt-3" data-wf-install-plugin><i data-lucide="download"></i>Install Workflows</button></div></div>`;
      const side = $("[data-wf-side]");
      if (side) side.hidden = true;
      window.lucide?.createIcons();
      return;
    }
    if (!wfAccountName()) {
      const main = $("[data-wf-main]");
      if (main) main.innerHTML = `<div class="wf-signed-out"><div><p class="text-sm font-semibold">${escapeHtml(WF_ERROR_TEXT.unauthorized)}</p><p class="wf-help">Workflows connect triggers, logic and 100 integrations on a visual canvas, and edit your repositories' actions, shortcuts and special agents.</p></div></div>`;
      const list = $("[data-wf-list]");
      if (list) list.innerHTML = "";
      return;
    }
    wfRenderLayout();
    wfRenderSide();
    wfRenderToolbar();
    wfRenderCanvas();
    wfRenderPanel();
    wfRenderRuns();
    wfRenderRail();
    wfSetNotice(view.notice, view.noticeTone);
    window.lucide?.createIcons();
  }

  function wfDraft() {
    return {
      mode: "automation",
      id: "",
      name: "My workflow",
      active: false,
      hookPath: "",
      nodes: [{
        id: wfNodeId(), type: "core.manual", name: "When clicking Run", x: 0, y: 0,
        params: {}, credential: "", continueOnFail: false, disabled: false,
      }],
      connections: [],
    };
  }

  function wfConfirmDiscard() {
    if (!wfView().dirty) return true;
    return window.confirm("Discard the unsaved changes?");
  }

  function wfSetUrl(params) {
    const url = new URL(location.href);
    for (const key of ["id", "repo", "file"]) url.searchParams.delete(key);
    for (const [key, value] of Object.entries(params || {})) if (value) url.searchParams.set(key, value);
    history.replaceState(history.state, "", url.pathname + url.search);
  }

  function wfResetSelection() {
    const view = wfView();
    view.selected = "";
    view.selectedEdge = -1;
    view.run = null;
    view.runs = [];
    view.lastField = null;
    view.sheetOpen = false;
  }

  function wfCloseDrawers() {
    const view = wfView();
    view.listOpen = false;
    view.menuOpen = false;
    wfRenderLayout();
  }

  function wfLoadDraft(workflow, notice = "", tone = "good") {
    const view = wfView();
    view.current = {
      mode: "automation",
      id: "",
      name: workflow.name || "Imported workflow",
      active: false,
      hookPath: "",
      nodes: (workflow.nodes || []).map((node) => ({ ...node, params: { ...(node.params || {}) } })),
      connections: (workflow.connections || []).map((conn) => ({ ...conn })),
    };
    view.mode = "automations";
    view.dirty = true;
    wfResetSelection();
    wfSetUrl({});
    wfSetNotice(notice, tone);
    wfCloseDrawers();
    renderWorkflowsPage();
    wfFit();
  }

  function wfNewWorkflow() {
    if (!wfConfirmDiscard()) return;
    const view = wfView();
    view.current = wfDraft();
    view.mode = "automations";
    view.dirty = false;
    wfResetSelection();
    view.selected = view.current.nodes[0].id;
    view.panX = 120;
    view.panY = 160;
    view.zoom = 1;
    wfSetUrl({});
    wfSetNotice("");
    wfCloseDrawers();
    renderWorkflowsPage();
  }

  function wfFromServer(workflow) {
    return {
      mode: "automation",
      id: workflow.id,
      name: workflow.name,
      active: Boolean(workflow.active),
      hookPath: workflow.hookPath || "",
      nodes: (workflow.nodes || []).map((node) => ({ ...node, params: { ...(node.params || {}) } })),
      connections: (workflow.connections || []).map((conn) => ({ ...conn })),
    };
  }

  function wfUpsertSummary(workflow) {
    const view = wfView();
    const summary = {
      id: workflow.id,
      name: workflow.name,
      active: workflow.active,
      nodes: (workflow.nodes || []).length,
      lastRunAt: workflow.lastRunAt || 0,
      lastStatus: workflow.lastStatus || "",
    };
    const index = view.workflows.findIndex((item) => item.id === workflow.id);
    if (index >= 0) view.workflows[index] = { ...view.workflows[index], ...summary };
    else view.workflows.unshift(summary);
  }

  async function wfOpenWorkflow(id) {
    const view = wfView();
    if (!wfIsFileMode() && view.current?.id === id) {
      wfCloseDrawers();
      return;
    }
    if (!wfConfirmDiscard()) return;
    view.busy = true;
    wfRenderToolbar();
    try {
      const data = await fetchJson(`/api/workflows/${encodeURIComponent(id)}`, { fresh: true });
      view.current = wfFromServer(data.workflow);
      view.mode = "automations";
      view.dirty = false;
      wfResetSelection();
      wfSetUrl({ id });
      wfSetNotice("");
      wfCloseDrawers();
      if (view.runsOpen || wfHasDesktopSteps(view.current)) void wfLoadRuns();
    } catch (error) {
      wfSetNotice(wfErrorText(error, "That workflow could not be opened."), "bad");
    } finally {
      view.busy = false;
      renderWorkflowsPage();
      wfFit();
    }
  }

  function wfPayload() {
    const current = wfView().current;
    return {
      name: current.name,
      active: current.active,
      nodes: current.nodes,
      connections: current.connections,
    };
  }

  async function wfSave(options = {}) {
    if (wfIsFileMode()) return wfSaveFile();
    const view = wfView();
    if (!view.current || view.busy) return false;
    view.busy = true;
    wfRenderToolbar();
    try {
      const path = view.current.id ? `/api/workflows/${view.current.id}` : "/api/workflows";
      const data = await wfPost(path, wfPayload());
      const selected = view.selected;
      view.current = wfFromServer(data.workflow);
      view.selected = wfNode(selected) ? selected : "";
      view.dirty = false;
      wfUpsertSummary(data.workflow);
      wfSetUrl({ id: view.current.id });
      if (!options.quiet) wfSetNotice("Saved.", "good");
      return true;
    } catch (error) {
      wfSetNotice(wfErrorText(error, "The workflow could not be saved."), "bad");
      return false;
    } finally {
      view.busy = false;
      renderWorkflowsPage();
    }
  }

  async function wfRun() {
    const view = wfView();
    if (!view.current || wfIsFileMode()) return;
    if (!view.current.nodes.some((node) => wfIsTrigger(node.type))) {
      wfSetNotice(WF_ERROR_TEXT.no_trigger, "bad");
      return;
    }
    if (view.dirty || !view.current.id) {
      if (!(await wfSave({ quiet: true }))) return;
    }
    if (wfHasDesktopSteps(view.current)) return wfRunOnDesktop();
    view.busy = true;
    wfSetNotice("Running...", "good");
    wfRenderToolbar();
    try {
      const data = await wfPost(`/api/workflows/${view.current.id}/run`, {});
      view.run = data.run;
      wfUpsertSummary({ ...view.current, lastRunAt: data.run.startedAt, lastStatus: data.run.status });
      if (data.run.status === "success") {
        wfSetNotice(`Workflow ran in ${Math.max(0, data.run.finishedAt - data.run.startedAt)} ms with ${data.run.requests} request${data.run.requests === 1 ? "" : "s"}.`, "good");
      } else {
        wfSetNotice(`Workflow failed. ${data.run.error || ""}`.trim(), "bad");
        const failed = Object.entries(data.run.nodes || {}).find(([, entry]) => entry.status === "error");
        if (failed) view.selected = failed[0];
      }
      if (view.runsOpen) void wfLoadRuns();
    } catch (error) {
      wfSetNotice(wfErrorText(error, "The workflow could not run."), "bad");
    } finally {
      view.busy = false;
      renderWorkflowsPage();
    }
  }

  function wfDesktopErrorText(error) {
    const code = String(error?.code || "");
    const text = String(error?.message || "");
    if (text && text !== code) return text;
    return WF_DESKTOP_ERRORS[code] || desktopControlErrorText(error);
  }

  async function wfDesktopConnection() {
    if (!state.session?.sessionToken) throw new Error(WF_ERROR_TEXT.unauthorized);
    const hub = await desktopControlHub();
    const desktops = await hub.desktops();
    if (!desktops.length) throw new Error(`No ${desktopBrand()}desktop is linked to your account. Open the desktop app and sign in, then run this again.`);
    let failure = null;
    for (const desktop of desktops) {
      try {
        return await hub.connect(desktop.node);
      } catch (error) {
        failure = error;
      }
    }
    throw failure;
  }

  async function wfRunOnDesktop() {
    const view = wfView();
    const workflow = view.current;
    const order = wfRunOrder(workflow);
    const live = { startedAt: Date.now(), finishedAt: 0, nodes: {} };
    view.busy = true;
    view.run = null;
    view.live = live;
    view.selected = "";
    wfSetNotice("Connecting to your desktop...", "good");
    renderWorkflowsPage();
    wfStartRailClock();
    if (!view.runs.length) await wfLoadRuns();
    let connection = null;
    try {
      connection = await wfDesktopConnection();
    } catch (error) {
      wfStopRailClock();
      view.live = null;
      view.busy = false;
      wfSetNotice(wfDesktopErrorText(error), "bad");
      renderWorkflowsPage();
      return;
    }
    const report = {};
    let failed = "";
    let freed = 0;
    let free = null;
    let total = null;
    const settle = (node, entry, startedAt) => {
      report[node.id] = entry;
      live.nodes[node.id] = { ...entry, startedAt, finishedAt: Date.now() };
      renderWorkflowsPage();
    };
    for (const node of order) {
      const started = Date.now();
      if (wfIsTrigger(node.type)) {
        settle(node, { status: "success", ms: 0, outputs: { main: { count: 1, items: [{}] } } }, started);
        continue;
      }
      const fed = workflow.connections.some((conn) => conn.to === node.id && report[conn.from]?.status === "success");
      if (failed || !fed) {
        settle(node, { status: "skipped" }, started);
        continue;
      }
      if (node.disabled || !wfIsDesktopType(node.type)) {
        settle(node, { status: "success", ms: 0, outputs: { main: { count: 1, items: [{}] } } }, started);
        continue;
      }
      const step = wfCore(node.type).desktopStep;
      live.nodes[node.id] = { status: "running", startedAt: started };
      wfSetNotice(`${node.name}...`, "good");
      renderWorkflowsPage();
      try {
        const prepared = await connection.request("diag.prepare", { action: "freeDiskSpace", step }, 30 * 1000);
        const result = await connection.request("diag.run", { action: "freeDiskSpace", step, token: prepared.token }, WF_DESKTOP_STEP_TIMEOUT_MS);
        freed += Number(result.freedBytes) || 0;
        if (Number(result.totalBytes) > 0) {
          free = Number(result.freeBytes) || 0;
          total = Number(result.totalBytes);
        }
        const item = {
          step,
          freedBytes: Number(result.freedBytes) || 0,
          freed: wfBytes(result.freedBytes),
          items: Number(result.items) || 0,
          freeBytes: Number(result.freeBytes) || 0,
          free: wfBytes(result.freeBytes),
        };
        settle(node, {
          status: "success",
          ms: Date.now() - started,
          outputs: { main: { count: 1, items: [item] } },
          logs: Array.isArray(result.lines) ? result.lines.map(String) : [],
        }, started);
      } catch (error) {
        const text = wfDesktopErrorText(error);
        failed = `${node.name}: ${text}`;
        settle(node, { status: "error", ms: Date.now() - started, error: text }, started);
      }
    }
    live.finishedAt = Date.now();
    wfStopRailClock();
    const output = { freedBytes: freed };
    if (free !== null) {
      output.freeBytes = free;
      output.totalBytes = total;
    }
    try {
      const data = await wfPost(`/api/workflows/${workflow.id}/record`, {
        startedAt: live.startedAt, finishedAt: live.finishedAt, nodes: report, output,
      });
      view.run = data.run;
      wfUpsertSummary({ ...workflow, lastRunAt: data.run.startedAt, lastStatus: data.run.status });
      if (data.run.status === "success") wfSetNotice(`Done in ${wfDuration(live.finishedAt - live.startedAt)}. ${data.run.output?.text || ""}`.trim(), "good");
      else {
        wfSetNotice(`Workflow failed. ${data.run.error || ""}`.trim(), "bad");
        const broken = Object.entries(data.run.nodes || {}).find(([, entry]) => entry.status === "error");
        if (broken) view.selected = broken[0];
      }
    } catch (error) {
      view.run = { status: failed ? "error" : "success", error: failed, nodes: report, startedAt: live.startedAt, finishedAt: live.finishedAt, trigger: "desktop" };
      wfSetNotice(`The run finished but could not be saved: ${wfErrorText(error, "try again")}`, "bad");
    }
    view.live = null;
    view.busy = false;
    view.runsOpen = true;
    renderWorkflowsPage();
    await wfLoadRuns();
    wfRenderRail();
  }

  async function wfDelete() {
    const view = wfView();
    const current = view.current;
    if (!current?.id || wfIsFileMode()) return;
    if (!window.confirm(`Delete "${current.name}" and its execution history?`)) return;
    view.busy = true;
    try {
      await wfPost(`/api/workflows/${current.id}/delete`, {});
      view.workflows = view.workflows.filter((item) => item.id !== current.id);
      view.dirty = false;
      view.current = null;
      wfResetSelection();
      wfSetNotice("Workflow deleted.", "good");
      view.busy = false;
      if (view.workflows.length) await wfOpenWorkflow(view.workflows[0].id);
      else wfNewWorkflow();
    } catch (error) {
      wfSetNotice(wfErrorText(error, "The workflow could not be deleted."), "bad");
    } finally {
      view.busy = false;
      renderWorkflowsPage();
    }
  }

  async function wfLoadRuns() {
    const view = wfView();
    if (!view.current?.id || wfIsFileMode()) return;
    try {
      const data = await fetchJson(`/api/workflows/${view.current.id}/runs`, { fresh: true });
      view.runs = Array.isArray(data.runs) ? data.runs : [];
    } catch (error) {
      view.runs = [];
      wfSetNotice(wfErrorText(error, "Executions are unavailable right now."), "bad");
    }
    wfRenderRuns();
    wfRenderRail();
  }

  async function wfOpenRun(runId) {
    const view = wfView();
    if (!view.current?.id) return;
    try {
      const data = await fetchJson(`/api/workflows/${view.current.id}/runs/${runId}`, { fresh: true });
      view.run = data.run;
      renderWorkflowsPage();
    } catch (error) {
      wfSetNotice(wfErrorText(error, "That execution could not be loaded."), "bad");
    }
  }

  function wfDownload(filename, text) {
    const blob = new Blob([text], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    link.remove();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  async function wfExport() {
    const view = wfView();
    if (!view.current || wfIsFileMode()) return;
    try {
      const data = await wfPost("/api/workflows/export", wfPayload());
      wfDownload(data.filename || "workflow.json", `${JSON.stringify(data.document, null, 2)}\n`);
      wfSetNotice(`Downloaded ${data.filename}. It opens in n8n too; integration steps stay ${platformName()} steps.`, "good");
    } catch (error) {
      wfSetNotice(wfErrorText(error, "The workflow could not be exported."), "bad");
    }
  }

  function wfImportNotice(prefix, warnings) {
    const list = Array.isArray(warnings) ? warnings : [];
    if (!list.length) return { text: `${prefix} Check the steps, then press Save.`, tone: "good" };
    return { text: `${prefix} Check these before saving:\n• ${list.slice(0, 8).join("\n• ")}${list.length > 8 ? `\n• and ${list.length - 8} more` : ""}`, tone: "warn" };
  }

  async function wfImportText(text, label) {
    if (!wfConfirmDiscard()) return;
    try {
      const data = await wfPost("/api/workflows/convert", { document: text });
      const notice = wfImportNotice(`Imported ${label}.`, data.warnings);
      wfLoadDraft(data.workflow, notice.text, notice.tone);
    } catch (error) {
      wfSetNotice(wfErrorText(error, "That file could not be imported."), "bad");
    }
  }

  async function wfImportUrl(url) {
    if (!wfConfirmDiscard()) return;
    try {
      const data = await wfPost("/api/workflows/import-url", { url });
      const notice = wfImportNotice("Imported the workflow from the link.", data.warnings);
      wfCloseDialog();
      wfLoadDraft(data.workflow, notice.text, notice.tone);
    } catch (error) {
      const message = $("[data-wf-import-error]");
      if (message) message.textContent = wfErrorText(error, "That link could not be imported.");
    }
  }

  function wfDuplicate() {
    const view = wfView();
    if (!view.current || wfIsFileMode()) return;
    const copy = JSON.parse(JSON.stringify(wfPayload()));
    copy.name = `${copy.name} copy`.slice(0, 80);
    wfLoadDraft(copy, "This is a copy. Press Save to keep it.", "good");
  }

  function wfOpenDialog(title, icon, body, wide = false) {
    const root = $("[data-wf-dialog]");
    if (!root) return;
    $("[data-wf-dialog-title]").textContent = title;
    const iconEl = $("[data-wf-dialog-icon]");
    if (iconEl) iconEl.outerHTML = `<i data-wf-dialog-icon data-lucide="${escapeHtml(icon)}"></i>`;
    $("[data-wf-dialog-box]")?.classList.toggle("wf-dialog-wide", wide);
    $("[data-wf-dialog-body]").innerHTML = body;
    root.hidden = false;
    window.lucide?.createIcons();
  }

  function wfCloseDialog() {
    const root = $("[data-wf-dialog]");
    if (root) root.hidden = true;
  }

  async function wfOpenTemplates() {
    const view = wfView();
    wfCloseDrawers();
    if (!view.templates.length) {
      try {
        const data = await fetchJson("/api/workflows/templates");
        view.templates = Array.isArray(data.templates) ? data.templates : [];
      } catch (error) {
        wfSetNotice(wfErrorText(error, "The examples are unavailable right now."), "bad");
        return;
      }
    }
    const cards = view.templates.map((item) => `<button type="button" class="wf-template" data-wf-template="${escapeHtml(item.key)}">
      <span class="text-sm font-semibold">${escapeHtml(item.name)}</span>
      <span class="wf-help mt-0">${escapeHtml(item.description)}</span>
      <span class="wf-template-flow">${(item.tags || []).map((tag, index) => `${index ? '<i data-lucide="chevron-right"></i>' : ""}<span class="wf-chip">${escapeHtml(tag)}</span>`).join("")}</span>
      <span class="wf-help mt-0">${item.steps} steps</span>
    </button>`).join("");
    wfOpenDialog("Start from an example", "layout-template", `<p class="wf-help px-2">Each example opens as a new workflow. Add your credentials and IDs, then save it.</p><div class="wf-templates">${cards}</div>`, true);
  }

  async function wfUseTemplate(key) {
    if (!wfConfirmDiscard()) return;
    try {
      const data = await fetchJson(`/api/workflows/templates/${encodeURIComponent(key)}`);
      wfCloseDialog();
      wfLoadDraft(data.template.workflow, `Opened the "${data.template.name}" example. Add your credentials and IDs, then press Save.`, "good");
    } catch (error) {
      wfSetNotice(wfErrorText(error, "That example could not be opened."), "bad");
    }
  }

  function wfOpenImportUrl() {
    wfCloseDrawers();
    wfOpenDialog("Import from a URL", "link", `<form data-wf-import-form class="p-2">
      <label class="wf-label mt-0" for="wf-import-url">Link to a workflow JSON file</label>
      <input id="wf-import-url" data-wf-import-input class="wf-input" type="url" required placeholder="https://n8n.io/workflows/1234 or a raw .json link" />
      <p class="wf-help">Works with n8n template pages, GitHub and Gist links, and any public https:// link to an n8n or ForkMesh workflow file. The workflow opens unsaved so you can check it first.</p>
      <p data-wf-import-error class="wf-help" style="color:#dc2626" aria-live="polite"></p>
      <button type="submit" class="wf-btn wf-btn-primary mt-3"><i data-lucide="download"></i>Import</button>
    </form>`);
    window.setTimeout(() => $("[data-wf-import-input]")?.focus(), 30);
  }

  function wfCreatesCycle(from, to, skip = -1) {
    const connections = wfView().current.connections.filter((_, index) => index !== skip);
    const stack = [to];
    const seen = new Set();
    while (stack.length) {
      const id = stack.pop();
      if (id === from) return true;
      if (seen.has(id)) continue;
      seen.add(id);
      for (const conn of connections) if (conn.from === id) stack.push(conn.to);
    }
    return false;
  }

  function wfCanConnect(from, output, to, skip = -1) {
    const current = wfView().current;
    const source = wfNode(from);
    const target = wfNode(to);
    if (!source || !target || from === to) return false;
    if (!wfSpec(target.type).inputs) return false;
    if (!wfOutputs(source.type).includes(output)) return false;
    if (current.connections.some((c, index) => index !== skip && c.from === from && c.to === to && c.output === output)) return false;
    if (wfCreatesCycle(from, to, skip)) {
      wfSetNotice(WF_ERROR_TEXT.cycle, "bad");
      return false;
    }
    return true;
  }

  function wfConnect(from, output, to) {
    if (!wfCanConnect(from, output, to)) return false;
    wfView().current.connections.push({ from, output, to });
    return true;
  }

  function wfViewportCenter() {
    const view = wfView();
    const canvas = $("[data-wf-canvas]");
    const rect = canvas?.getBoundingClientRect() || { width: 800, height: 500 };
    return {
      x: (rect.width / 2 - view.panX) / view.zoom - WF_NODE_W / 2,
      y: (rect.height / 2 - view.panY) / view.zoom - WF_NODE_H / 2,
    };
  }

  function wfSnap(value) {
    return Math.round(value / WF_GRID) * WF_GRID;
  }

  function wfDefaults(spec) {
    const params = {};
    for (const field of spec.fields || []) {
      if (field.type === "checks") params[field.key] = [];
      else if (field.default !== undefined) params[field.key] = field.default;
    }
    return params;
  }

  function wfFileRoot() {
    return (wfView().current?.nodes || []).find((node) => wfIsRoot(node)) || null;
  }

  function wfFileAutoTarget(spec) {
    const root = wfFileRoot();
    if (!root || spec.inputs || !spec.outputs.length || !wfSpec(root.type).inputs) return null;
    return root;
  }

  function wfFileTail() {
    const current = wfView().current;
    const root = wfFileRoot();
    if (!root) return null;
    if (current.kind === "agent") return root;
    let node = root;
    const seen = new Set();
    for (;;) {
      seen.add(node.id);
      const next = current.connections
        .filter((conn) => conn.from === node.id)
        .map((conn) => wfNode(conn.to))
        .filter((item) => item && !seen.has(item.id))
        .sort((a, b) => (b.x - a.x) || (b.y - a.y))[0];
      if (!next) return node;
      node = next;
    }
  }

  function wfAddNode(type, after) {
    const view = wfView();
    const current = view.current;
    if (!current) return;
    const spec = wfSpec(type);
    if (spec.kind === "unknown") return;
    if (spec.single && current.nodes.some((node) => node.type === type)) {
      wfSetNotice(spec.kind === "core" ? WF_ERROR_TEXT.too_many_triggers : `This file can have only one ${spec.name.toLowerCase()}.`, "bad");
      return;
    }
    let anchor = after;
    if (!anchor && view.selected && spec.inputs) {
      const selected = wfNode(view.selected);
      if (selected && wfOutputs(selected.type).length) anchor = { from: selected.id, output: wfOutputs(selected.type)[0] };
    }
    const into = !anchor && wfIsFileMode() ? wfFileAutoTarget(spec) : null;
    if (!anchor && wfIsFileMode() && !into && spec.inputs) {
      const tail = wfFileTail();
      if (tail) anchor = { from: tail.id, output: wfOutputs(tail.type)[0] };
    }
    let position = anchor?.x !== undefined ? { x: anchor.x, y: anchor.y } : wfViewportCenter();
    if (into) {
      const stacked = current.nodes.filter((n) => current.connections.some((c) => c.from === n.id && c.to === into.id)).length;
      position = { x: into.x - WF_COLUMN, y: into.y + (stacked - 1) * WF_ROW };
    }
    const source = anchor?.from ? wfNode(anchor.from) : null;
    if (source && anchor.x === undefined) {
      const outputs = wfOutputs(source.type);
      const index = Math.max(0, outputs.indexOf(anchor.output));
      const siblings = current.connections.filter((c) => c.from === source.id && c.output === anchor.output).length;
      position = { x: source.x + WF_NODE_W + 72, y: source.y + (index - (outputs.length - 1) / 2) * 96 + siblings * 96 };
    }
    const node = {
      id: wfNodeId(),
      type,
      name: wfUniqueName(spec.name),
      x: wfSnap(position.x),
      y: wfSnap(position.y),
      params: spec.kind === "app" ? { operation: spec.app.operations[0]?.key || "" } : (spec.kind === "file" ? wfDefaults(spec) : {}),
      credential: "",
      continueOnFail: false,
      disabled: false,
    };
    if (spec.kind === "app" && spec.app.credential.length) {
      const creds = view.credentials.filter((cred) => cred.integration === spec.app.key);
      if (creds.length === 1) node.credential = creds[0].id;
    }
    current.nodes.push(node);
    if (source && spec.inputs) wfConnect(source.id, anchor.output, node.id);
    if (into) wfConnect(node.id, wfOutputs(node.type)[0], into.id);
    view.selected = node.id;
    view.selectedEdge = -1;
    wfMarkDirty();
    wfClosePalette();
    if (wfIsSmall()) view.sheetOpen = true;
    renderWorkflowsPage();
  }

  function wfDeleteNode(id) {
    const view = wfView();
    const current = view.current;
    if (!current) return;
    const node = wfNode(id);
    if (node && wfIsFileMode() && wfIsRoot(node)) return;
    current.nodes = current.nodes.filter((item) => item.id !== id);
    current.connections = current.connections.filter((conn) => conn.from !== id && conn.to !== id);
    if (view.selected === id) view.selected = "";
    view.selectedEdge = -1;
    wfMarkDirty();
    renderWorkflowsPage();
  }

  function wfDeleteEdge(index) {
    const view = wfView();
    if (!view.current?.connections[index]) return;
    view.current.connections.splice(index, 1);
    view.selectedEdge = -1;
    wfMarkDirty();
    renderWorkflowsPage();
  }

  function wfRenameNode(node, name) {
    const clean = String(name || "").trim().slice(0, 60);
    if (!clean || clean === node.name) return;
    if (wfView().current.nodes.some((other) => other.id !== node.id && other.name === clean)) {
      wfSetNotice(WF_ERROR_TEXT.duplicate_node_name, "bad");
      return;
    }
    const before = JSON.stringify(node.name);
    const after = JSON.stringify(clean);
    for (const other of wfView().current.nodes) {
      for (const [key, value] of Object.entries(other.params || {})) {
        if (typeof value === "string" && value.includes("$node[")) {
          other.params[key] = value.split(`$node[${before}]`).join(`$node[${after}]`);
        }
      }
    }
    node.name = clean;
    wfMarkDirty();
  }

  function wfTidy() {
    const view = wfView();
    const current = view.current;
    if (!current?.nodes.length) return;
    const remaining = new Map(current.nodes.map((n) => [n.id, 0]));
    for (const conn of current.connections) remaining.set(conn.to, (remaining.get(conn.to) || 0) + 1);
    const depth = new Map();
    const queue = current.nodes.filter((n) => !remaining.get(n.id)).map((n) => n.id);
    queue.forEach((id) => depth.set(id, 0));
    while (queue.length) {
      const id = queue.shift();
      for (const conn of current.connections.filter((c) => c.from === id)) {
        depth.set(conn.to, Math.max(depth.get(conn.to) || 0, (depth.get(id) || 0) + 1));
        remaining.set(conn.to, remaining.get(conn.to) - 1);
        if (remaining.get(conn.to) === 0) queue.push(conn.to);
      }
    }
    for (const node of current.nodes) if (!depth.has(node.id)) depth.set(node.id, 0);
    const rows = new Map();
    const sorted = [...current.nodes].sort((a, b) => (depth.get(a.id) - depth.get(b.id)) || (a.y - b.y) || (a.x - b.x));
    for (const node of sorted) {
      const column = depth.get(node.id);
      const row = rows.get(column) || 0;
      rows.set(column, row + 1);
      node.x = column * WF_COLUMN;
      node.y = row * WF_ROW;
    }
    for (const [column, count] of rows) {
      const shift = ((count - 1) * WF_ROW) / 2;
      for (const node of current.nodes) if (depth.get(node.id) === column) node.y -= shift;
    }
    wfMarkDirty();
    wfRenderCanvas();
    wfFit();
  }

  function wfFit(render = true) {
    const view = wfView();
    const nodes = view.current?.nodes || [];
    const canvas = $("[data-wf-canvas]");
    const rect = canvas?.getBoundingClientRect();
    if (!nodes.length || !rect?.width) {
      view.zoom = 1;
      view.panX = 120;
      view.panY = 160;
    } else {
      const minX = Math.min(...nodes.map((n) => n.x));
      const minY = Math.min(...nodes.map((n) => n.y));
      const maxX = Math.max(...nodes.map((n) => n.x + WF_NODE_W));
      const maxY = Math.max(...nodes.map((n) => n.y + WF_NODE_H));
      const margin = wfIsSmall() ? 48 : 120;
      const zoom = Math.min(1.2, Math.max(0.25, Math.min((rect.width - margin) / (maxX - minX || 1), (rect.height - margin) / (maxY - minY || 1))));
      view.zoom = zoom;
      view.panX = (rect.width - (maxX - minX) * zoom) / 2 - minX * zoom;
      view.panY = (rect.height - (maxY - minY) * zoom) / 2 - minY * zoom;
    }
    if (render) wfApplyTransform();
  }

  function wfZoomAt(factor, clientX, clientY) {
    const view = wfView();
    const canvas = $("[data-wf-canvas]");
    const rect = canvas.getBoundingClientRect();
    const px = clientX === undefined ? rect.width / 2 : clientX - rect.left;
    const py = clientY === undefined ? rect.height / 2 : clientY - rect.top;
    const zoom = Math.min(2, Math.max(0.25, view.zoom * factor));
    view.panX = px - ((px - view.panX) * zoom) / view.zoom;
    view.panY = py - ((py - view.panY) * zoom) / view.zoom;
    view.zoom = zoom;
    wfApplyTransform();
  }

  function wfWorldPoint(event) {
    const view = wfView();
    const rect = $("[data-wf-canvas]").getBoundingClientRect();
    return {
      x: (event.clientX - rect.left - view.panX) / view.zoom,
      y: (event.clientY - rect.top - view.panY) / view.zoom,
    };
  }

  function wfSelect(id) {
    const view = wfView();
    view.selected = id;
    view.selectedEdge = -1;
    view.lastField = null;
    for (const el of $$("[data-wf-node]")) el.classList.toggle("is-selected", el.dataset.wfNode === id);
    wfRenderEdges();
    wfRenderPanel();
  }

  function wfPaletteEntries() {
    const view = wfView();
    const query = view.paletteQuery.trim().toLowerCase();
    const category = view.paletteCategory;
    const present = new Set((view.current?.nodes || []).map((node) => node.type));
    const entries = [];
    if (wfIsFileMode()) {
      for (const spec of view.catalog?.files?.nodes || []) {
        if (spec.kind !== view.current.kind) continue;
        if (spec.single && present.has(spec.type)) continue;
        entries.push({ type: spec.type, name: spec.name, description: spec.description, group: spec.group });
      }
    } else {
      const hasTrigger = (view.current?.nodes || []).some((node) => wfIsTrigger(node.type));
      for (const core of view.catalog?.core || []) {
        if (core.trigger && hasTrigger) continue;
        entries.push({ type: core.type, name: core.name, description: core.description, group: core.group });
      }
      for (const app of wfCatalogApps().filter(wfAppOn)) {
        entries.push({ type: app.type, name: app.name, description: app.description, group: app.category, actions: app.operations.map((op) => op.name).join(" ") });
      }
    }
    return entries.filter((entry) => {
      if (category && entry.group !== category) return false;
      if (!query) return true;
      return `${entry.name} ${entry.description} ${entry.group} ${entry.actions || ""}`.toLowerCase().includes(query);
    });
  }

  function wfRenderPalette() {
    const view = wfView();
    const cats = $("[data-wf-palette-cats]");
    const list = $("[data-wf-palette-list]");
    if (!cats || !list) return;
    const all = wfIsFileMode()
      ? Array.from(new Set((view.catalog?.files?.nodes || []).filter((spec) => spec.kind === view.current.kind).map((spec) => spec.group)))
      : WF_CORE_GROUPS.concat(view.catalog?.categories || []);
    cats.hidden = all.length < 2;
    cats.innerHTML = [`<button type="button" class="wf-cat" data-wf-cat="" aria-pressed="${view.paletteCategory ? "false" : "true"}">All</button>`]
      .concat(all.map((group) => `<button type="button" class="wf-cat" data-wf-cat="${escapeHtml(group)}" aria-pressed="${view.paletteCategory === group ? "true" : "false"}">${WF_CATEGORY_ICONS[group] ? `<i data-lucide="${WF_CATEGORY_ICONS[group]}"></i>` : ""}${escapeHtml(group)}</button>`))
      .join("");
    const entries = wfPaletteEntries();
    if (!entries.length) {
      list.innerHTML = `<p class="wf-help px-3 py-3">${wfIsFileMode() ? "Every step this file can have is already on the canvas." : "Nothing matches. Use the HTTP request step to call any other HTTPS API."}</p>`;
      return;
    }
    const byGroup = new Map();
    for (const entry of entries) {
      if (!byGroup.has(entry.group)) byGroup.set(entry.group, []);
      byGroup.get(entry.group).push(entry);
    }
    const order = all.filter((group) => byGroup.has(group));
    list.innerHTML = order.map((group) => `<p class="wf-group-title">${escapeHtml(group)}</p>${byGroup.get(group).map((entry) => `<button type="button" class="wf-pick" data-wf-pick="${escapeHtml(entry.type)}">${wfBadge(entry.type, true)}<span class="min-w-0"><span class="wf-pick-name block">${escapeHtml(entry.name)}</span><span class="wf-pick-desc block truncate">${escapeHtml(entry.description)}</span></span></button>`).join("")}`).join("");
    window.lucide?.createIcons();
  }

  function wfOpenPalette(after = null) {
    const view = wfView();
    if (!view.current || !view.catalog) return;
    view.paletteAfter = after;
    view.paletteQuery = "";
    view.paletteCategory = "";
    const root = $("[data-wf-palette]");
    if (!root) return;
    root.hidden = false;
    const search = $("[data-wf-palette-search]");
    if (search) {
      search.value = "";
      if (!wfIsSmall()) search.focus();
    }
    wfRenderPalette();
  }

  function wfClosePalette() {
    const root = $("[data-wf-palette]");
    if (root) root.hidden = true;
    wfView().paletteAfter = null;
  }

  function wfCredentialFormHtml() {
    const view = wfView();
    const apps = wfCatalogApps().filter((app) => app.credential.length);
    const selected = apps.find((app) => app.key === view.credentialIntegration) || null;
    const options = ['<option value="">Choose an integration</option>'].concat(apps.map((app) => `<option value="${escapeHtml(app.key)}"${selected?.key === app.key ? " selected" : ""}>${escapeHtml(app.name)}</option>`)).join("");
    const fields = selected ? selected.credential.map((field) => `<label class="wf-label" for="wf-cred-${escapeHtml(field.key)}">${escapeHtml(field.label)}</label><input id="wf-cred-${escapeHtml(field.key)}" class="wf-input" name="${escapeHtml(field.key)}" type="${field.secret ? "password" : "text"}" autocomplete="off" placeholder="${escapeHtml(field.placeholder || "")}" />`).join("") : "";
    return `<form data-wf-credential-form class="wf-section">
      <p class="wf-label mt-0">Add a credential</p>
      <label class="wf-label" for="wf-cred-integration">Integration</label>
      <select id="wf-cred-integration" class="wf-select" data-wf-credential-integration>${options}</select>
      ${selected ? `<label class="wf-label" for="wf-cred-name">Name</label><input id="wf-cred-name" class="wf-input" name="__name" type="text" maxlength="60" value="${escapeHtml(selected.name)} account" />${fields}<p class="wf-help">Stored encrypted and only sent to ${escapeHtml(selected.host || "the address you enter")}. It is never shown again after saving.</p><button type="submit" class="wf-btn wf-btn-primary mt-3"><i data-lucide="key-round"></i>Save credential</button>` : ""}
    </form>`;
  }

  function wfRenderCredentials() {
    const view = wfView();
    const root = $("[data-wf-credentials-body]");
    if (!root) return;
    const list = view.credentials.length
      ? view.credentials.map((cred) => {
        const app = wfApp(`app.${cred.integration}`);
        const hint = Object.entries(cred.hint || {}).map(([key, value]) => `${key}: ${value}`).join(", ");
        return `<div class="wf-cred">${wfBadge(`app.${cred.integration}`, true)}<div class="min-w-0 flex-1"><div class="font-semibold truncate">${escapeHtml(cred.name)}</div><div class="wf-help mt-0 truncate">${escapeHtml(app?.name || cred.integration)}${hint ? ` · ${escapeHtml(hint)}` : ""}</div></div><button type="button" class="wf-icon-btn" data-wf-delete-credential="${escapeHtml(cred.id)}" aria-label="Delete ${escapeHtml(cred.name)}"><i data-lucide="trash-2"></i></button></div>`;
      }).join("")
      : '<p class="wf-help">No credentials yet. Add API keys and tokens here, then pick them on a step.</p>';
    root.innerHTML = `<div class="pt-2">${list}</div>${wfCredentialFormHtml()}`;
    window.lucide?.createIcons();
  }

  function wfOpenCredentials(integration = "") {
    const view = wfView();
    view.credentialIntegration = integration;
    wfCloseDrawers();
    const root = $("[data-wf-credentials]");
    if (!root) return;
    root.hidden = false;
    wfRenderCredentials();
  }

  function wfCloseCredentials() {
    const root = $("[data-wf-credentials]");
    if (root) root.hidden = true;
  }

  async function wfSaveCredential(form) {
    const view = wfView();
    const integration = view.credentialIntegration;
    const values = {};
    for (const input of form.querySelectorAll("input[name]")) {
      if (input.name !== "__name") values[input.name] = input.value;
    }
    const name = form.querySelector('input[name="__name"]')?.value || "";
    try {
      const data = await wfPost("/api/workflows/credentials", { integration, name, values });
      view.credentials.unshift(data.credential);
      const node = view.selected ? wfNode(view.selected) : null;
      if (node && node.type === `app.${integration}` && !node.credential) {
        node.credential = data.credential.id;
        wfMarkDirty();
      }
      view.credentialIntegration = "";
      wfCloseCredentials();
      wfSetNotice(`Credential "${data.credential.name}" saved.`, "good");
      wfRenderPanel();
    } catch (error) {
      wfSetNotice(wfErrorText(error, "The credential could not be saved."), "bad");
    }
  }

  async function wfDeleteCredential(id) {
    const view = wfView();
    const cred = view.credentials.find((item) => item.id === id);
    if (!cred || !window.confirm(`Delete the credential "${cred.name}"? Steps using it will stop working.`)) return;
    try {
      await wfPost(`/api/workflows/credentials/${id}/delete`, {});
      view.credentials = view.credentials.filter((item) => item.id !== id);
      wfRenderCredentials();
      wfRenderPanel();
    } catch (error) {
      wfSetNotice(wfErrorText(error, "The credential could not be deleted."), "bad");
    }
  }

  function wfRepoBase(repo) {
    return `/api/repo/${encodeURIComponent(repo.owner)}/${encodeURIComponent(repo.name)}`;
  }

  function wfParseRepo(text) {
    const match = /^\s*([A-Za-z0-9_.-]+)\/([A-Za-z0-9_.-]+)\s*$/.exec(String(text || "").replace(/^https?:\/\/[^/]+\//, "").replace(/\.git$/, ""));
    return match ? { owner: match[1], name: match[2] } : null;
  }

  async function wfTreeNames(repo, folder) {
    try {
      const tree = await fetchRepoJson(`${wfRepoBase(repo)}/tree?path=${encodeURIComponent(folder)}&ref=${encodeURIComponent(repo.branch)}`);
      return (Array.isArray(tree?.entries) ? tree.entries : [])
        .filter((entry) => entry?.type !== "tree")
        .map((entry) => `${folder}/${String(entry?.name || "")}`);
    } catch {
      return [];
    }
  }

  async function wfOpenRepo(text, options = {}) {
    const view = wfView();
    const parsed = wfParseRepo(text);
    if (!parsed) {
      wfSetNotice("Enter a repository as owner/name.", "bad");
      return false;
    }
    view.repoLoading = true;
    view.repo = { ...parsed, branch: "" };
    wfRenderFiles();
    try {
      let branch = "";
      try {
        const branches = await fetchRepoJson(`${wfRepoBase(parsed)}/branches`);
        branch = String(branches?.defaultBranch || "").trim();
      } catch {
        branch = "";
      }
      view.repo.branch = branch || "main";
      const kinds = view.catalog?.files?.kinds || {};
      const folders = Array.from(new Set(Object.values(kinds).map((kind) => kind.folder)));
      const listed = await Promise.all(folders.map((folder) => wfTreeNames(view.repo, folder)));
      const all = listed.flat();
      const files = {};
      for (const [kind, meta] of Object.entries(kinds)) {
        const pattern = new RegExp(meta.pattern);
        files[kind] = all.filter((path) => pattern.test(path)).sort();
      }
      view.repoFiles = files;
      try {
        const shipped = await fetchJson("/api/workflows/package-files", { fresh: true });
        view.packageFiles = Array.isArray(shipped.files) ? shipped.files : [];
      } catch {
        view.packageFiles = [];
      }
      if (!all.length && !options.quiet) wfSetNotice(`No .forkmesh files found on ${view.repo.branch}. Use + to create one.`, "warn");
      return true;
    } catch (error) {
      view.repoFiles = null;
      wfSetNotice(wfErrorText(error, "That repository could not be read."), "bad");
      return false;
    } finally {
      view.repoLoading = false;
      wfRenderFiles();
    }
  }

  function wfLoadFileDocument(document, extra, warnings) {
    const view = wfView();
    view.current = {
      mode: "file",
      kind: document.kind,
      path: document.path,
      name: document.name,
      meta: document.meta || {},
      nodes: document.nodes.map((node) => ({ ...node, params: { ...(node.params || {}) } })),
      connections: document.connections.map((conn) => ({ ...conn })),
      oid: extra.oid || "",
      isNew: Boolean(extra.isNew),
    };
    view.mode = "files";
    view.dirty = Boolean(extra.isNew);
    wfResetSelection();
    wfSetUrl({ repo: `${view.repo.owner}/${view.repo.name}`, file: document.path });
    const list = Array.isArray(warnings) ? warnings : [];
    wfSetNotice(list.length ? `Opened ${document.path}. Note:\n• ${list.join("\n• ")}` : "", list.length ? "warn" : "good");
    wfCloseDrawers();
    renderWorkflowsPage();
    wfFit();
  }

  async function wfOpenFile(path) {
    const view = wfView();
    if (!view.repo) return;
    if (wfIsFileMode() && view.current?.path === path && !view.current.isNew) {
      wfCloseDrawers();
      return;
    }
    if (!wfConfirmDiscard()) return;
    view.busy = true;
    wfRenderToolbar();
    try {
      const blob = await fetchRepoJson(`${wfRepoBase(view.repo)}/blob?path=${encodeURIComponent(path)}&ref=${encodeURIComponent(view.repo.branch)}`);
      const text = blobText(blob);
      const oid = String(blob?.oid || blob?.sha || blob?.blob || "");
      const data = await wfPost("/api/workflows/files/parse", { path, text });
      wfLoadFileDocument(data.document, { oid }, data.warnings);
    } catch (error) {
      wfSetNotice(wfErrorText(error, `${path} could not be opened.`), "bad");
    } finally {
      view.busy = false;
      renderWorkflowsPage();
    }
  }

  async function wfNewFile(kind) {
    const view = wfView();
    if (!view.repo) return;
    const meta = view.catalog?.files?.kinds?.[kind];
    if (!meta) return;
    const examples = { action: "nightly-tests.yml", shortcut: "release-checklist.md or deploy.sh", skill: "code-review.skill", agent: "release-manager" };
    const answer = window.prompt(`File name for the new ${meta.label.toLowerCase()} (${examples[kind]})`, "");
    if (!answer) return;
    let file = answer.trim().replace(/\s+/g, "-");
    if (kind === "agent") file = `${file.toLowerCase().replace(/\.json$/, "")}.json`;
    if (kind === "skill" && !file.endsWith(".skill")) file += ".skill";
    if (kind === "action" && !/\.ya?ml$/.test(file)) file += ".yml";
    const path = `${meta.folder}/${file}`;
    if ((view.repoFiles?.[kind] || []).includes(path)) {
      wfSetNotice(`${path} already exists. Open it from the list instead.`, "bad");
      return;
    }
    if (!wfConfirmDiscard()) return;
    try {
      const data = await wfPost("/api/workflows/files/new", { kind, path });
      wfLoadFileDocument(data.document, { isNew: true }, []);
      wfSetNotice(`New ${meta.label.toLowerCase()} ${path}. It is created in the repository when you save.`, "good");
    } catch (error) {
      wfSetNotice(wfErrorText(error, "That file name cannot be used."), "bad");
    }
  }

  async function wfAddPackageFile(index) {
    const view = wfView();
    const item = view.packageFiles?.[Number(index)];
    if (!item || !view.repo) return;
    if ((view.repoFiles?.[item.kind] || []).includes(item.path)) {
      await wfOpenFile(item.path);
      wfSetNotice(`${item.path} is already in the repository. Opened it instead.`, "good");
      return;
    }
    if (!wfConfirmDiscard()) return;
    try {
      const data = await wfPost("/api/workflows/files/parse", { path: item.path, text: item.text });
      wfLoadFileDocument(data.document, { isNew: true }, data.warnings);
      wfSetNotice(`${item.name} from ${item.packageName} is ready. Check it, then use Save to repository to add ${item.path}.`, "good");
    } catch (error) {
      wfSetNotice(wfErrorText(error, "That file could not be prepared."), "bad");
    }
  }

  function wfFileDocument() {
    const current = wfView().current;
    return { kind: current.kind, path: current.path, meta: current.meta, nodes: current.nodes, connections: current.connections };
  }

  async function wfRenderFileText() {
    return wfPost("/api/workflows/files/render", { document: wfFileDocument() });
  }

  async function wfPreviewFile() {
    try {
      const data = await wfRenderFileText();
      const warnings = data.warnings?.length ? `<p class="wf-help" style="color:#b45309">${data.warnings.map(escapeHtml).join("<br>")}</p>` : "";
      wfOpenDialog(wfView().current.path, "file-code", `${warnings}<pre class="wf-pre" style="max-height:none">${escapeHtml(data.text)}</pre>`, true);
    } catch (error) {
      wfSetNotice(wfErrorText(error, "The file could not be built."), "bad");
    }
  }

  async function wfSaveFile() {
    const view = wfView();
    const current = view.current;
    if (!current || view.busy || !view.repo) return false;
    view.busy = true;
    wfRenderToolbar();
    try {
      const rendered = await wfRenderFileText();
      if (rendered.warnings?.length && !window.confirm(`${rendered.warnings.join("\n")}\n\nSave anyway?`)) return false;
      const body = {
        path: current.path,
        content: rendered.text,
        branch: view.repo.branch,
        message: `${current.isNew ? "Add" : "Update"} ${current.path} in the workflow builder`,
      };
      if (current.isNew) body.create = true;
      else if (current.oid) body.oid = current.oid;
      const token = String(state.session?.sessionToken || "");
      if (token && token !== "cookie") body.sessionToken = token;
      const response = await fetch(`${wfRepoBase(view.repo)}/file-edit`, {
        method: "POST",
        credentials: "same-origin",
        cache: "no-store",
        headers: wfAuthHeaders(),
        body: JSON.stringify(body),
      });
      const data = await response.json().catch(() => ({}));
      if (!response.ok || data?.ok === false) {
        const error = new Error(data?.message || data?.error || `HTTP ${response.status}`);
        error.code = data?.error || (response.status === 409 ? "conflict" : response.status === 403 ? "forbidden" : "");
        throw error;
      }
      const wasNew = current.isNew;
      current.oid = String(data.oid || current.oid || "");
      current.isNew = false;
      view.dirty = false;
      if (wasNew && view.repoFiles) {
        view.repoFiles[current.kind] = Array.from(new Set([...(view.repoFiles[current.kind] || []), current.path])).sort();
      }
      wfSetNotice(data.unchanged ? "Nothing changed in the file." : `Saved ${current.path} to ${view.repo.branch}${data.commit ? ` (commit ${String(data.commit).slice(0, 10)})` : ""}.`, "good");
      return true;
    } catch (error) {
      wfSetNotice(wfErrorText(error, error?.message || "The file could not be saved."), "bad");
      return false;
    } finally {
      view.busy = false;
      renderWorkflowsPage();
    }
  }

  function wfSetMode(mode) {
    const view = wfView();
    if (view.mode === mode) return;
    view.mode = mode;
    wfRenderSide();
    if (mode === "files" && !view.repoOptions.length) void wfLoadRepoOptions();
  }

  async function wfLoadRepoOptions() {
    const view = wfView();
    try {
      const data = await fetchJson("/api/repositories");
      const me = wfAccountName();
      const names = (Array.isArray(data.repositories) ? data.repositories : [])
        .filter((repo) => String(repo?.owner || "").toLowerCase() === me)
        .map((repo) => `${repo.owner}/${repo.name}`);
      view.repoOptions = Array.from(new Set(names)).sort().slice(0, 200);
      if (!view.repo && view.repoOptions.length === 1) {
        const input = $("[data-wf-repo-input]");
        if (input && !input.value) input.value = view.repoOptions[0];
      }
    } catch {
      view.repoOptions = [];
    }
    wfRenderFiles();
  }

  function wfInsertExpression(key) {
    const view = wfView();
    const field = view.lastField;
    const el = field ? $(`[data-wf-panel-body] [data-wf-param="${CSS.escape(field)}"]`) : null;
    if (!el || el.type === "checkbox" || el.tagName === "SELECT") {
      wfSetNotice("Click into a field first, then pick the input field to insert.", "bad");
      return;
    }
    const safeKey = /^[A-Za-z_$][\w$]*$/.test(key) ? `.${key}` : `[${JSON.stringify(key)}]`;
    const text = `{{ $json${safeKey} }}`;
    const start = el.selectionStart ?? el.value.length;
    const end = el.selectionEnd ?? el.value.length;
    el.value = el.value.slice(0, start) + text + el.value.slice(end);
    el.focus();
    el.selectionStart = el.selectionEnd = start + text.length;
    el.dispatchEvent(new Event("input", { bubbles: true }));
  }

  function wfNodeAt(clientX, clientY) {
    return document.elementFromPoint(clientX, clientY)?.closest("[data-wf-node]") || null;
  }

  function wfBindCanvas() {
    const canvas = $("[data-wf-canvas]");
    if (!canvas) return;
    let drag = null;
    let pressTimer = 0;
    const pointers = new Map();
    const cancelPress = () => {
      if (pressTimer) window.clearTimeout(pressTimer);
      pressTimer = 0;
    };
    const startPinch = () => {
      const view = wfView();
      const [a, b] = Array.from(pointers.values());
      drag = {
        kind: "pinch",
        distance: Math.hypot(a.x - b.x, a.y - b.y) || 1,
        midX: (a.x + b.x) / 2,
        midY: (a.y + b.y) / 2,
        zoom: view.zoom,
        panX: view.panX,
        panY: view.panY,
      };
      cancelPress();
      canvas.classList.remove("is-panning");
      wfRenderEdges();
    };
    canvas.addEventListener("pointerdown", (event) => {
      const view = wfView();
      if (!view.current || event.button > 0) return;
      if (event.target.closest("[data-wf-add-after], [data-wf-open-file], .wf-zoom, [data-wf-empty] button")) return;
      pointers.set(event.pointerId, { x: event.clientX, y: event.clientY });
      canvas.setPointerCapture(event.pointerId);
      if (pointers.size === 2) {
        startPinch();
        return;
      }
      if (pointers.size > 2) return;
      const handle = event.target.closest("[data-wf-edge-end]");
      if (handle) {
        const index = Number(handle.dataset.wfEdgeIndex);
        const conn = view.current.connections[index];
        if (conn) {
          const fixed = handle.dataset.wfEdgeEnd === "to"
            ? wfPortPoint(wfNode(conn.from), "out", conn.output)
            : wfPortPoint(wfNode(conn.to), "in");
          drag = { kind: "reconnect", index, end: handle.dataset.wfEdgeEnd, fixed };
          event.preventDefault();
          return;
        }
      }
      const outPort = event.target.closest("[data-wf-port-out]");
      if (outPort) {
        const node = wfNode(outPort.dataset.wfPortOut);
        drag = { kind: "connect", from: node.id, output: outPort.dataset.wfOutput, start: wfPortPoint(node, "out", outPort.dataset.wfOutput) };
        event.preventDefault();
        return;
      }
      const inPort = event.target.closest("[data-wf-port-in]");
      if (inPort) {
        const id = inPort.dataset.wfPortIn;
        let index = -1;
        view.current.connections.forEach((conn, i) => { if (conn.to === id) index = i; });
        if (index >= 0) {
          const conn = view.current.connections[index];
          drag = { kind: "reconnect", index, end: "to", fixed: wfPortPoint(wfNode(conn.from), "out", conn.output) };
          view.selectedEdge = index;
          event.preventDefault();
          return;
        }
      }
      const nodeEl = event.target.closest("[data-wf-node]");
      if (nodeEl) {
        const node = wfNode(nodeEl.dataset.wfNode);
        if (view.selected !== node.id) wfSelect(node.id);
        drag = { kind: "node", id: node.id, x: event.clientX, y: event.clientY, nodeX: node.x, nodeY: node.y, moved: false, el: nodeEl };
        return;
      }
      const edge = event.target.closest("[data-wf-edge]");
      if (edge) {
        view.selected = "";
        view.selectedEdge = Number(edge.dataset.wfEdge);
        for (const el of $$("[data-wf-node]")) el.classList.remove("is-selected");
        wfRenderEdges();
        wfRenderPanel();
        if (wfIsSmall()) {
          view.sheetOpen = true;
          wfRenderLayout();
        }
        return;
      }
      if (view.selected || view.selectedEdge >= 0) {
        view.selected = "";
        view.selectedEdge = -1;
        for (const el of $$("[data-wf-node]")) el.classList.remove("is-selected");
        wfRenderEdges();
        wfRenderPanel();
      }
      if (view.sheetOpen && wfIsSmall()) {
        view.sheetOpen = false;
        wfRenderLayout();
      }
      drag = { kind: "pan", x: event.clientX, y: event.clientY, panX: view.panX, panY: view.panY, moved: false };
      canvas.classList.add("is-panning");
      if (event.pointerType !== "mouse") {
        const point = wfWorldPoint(event);
        pressTimer = window.setTimeout(() => {
          pressTimer = 0;
          if (drag?.kind === "pan" && !drag.moved) {
            drag = null;
            canvas.classList.remove("is-panning");
            wfOpenPalette({ x: point.x - WF_NODE_W / 2, y: point.y - WF_NODE_H / 2 });
          }
        }, WF_LONG_PRESS_MS);
      }
    });
    canvas.addEventListener("pointermove", (event) => {
      if (!pointers.has(event.pointerId)) return;
      pointers.set(event.pointerId, { x: event.clientX, y: event.clientY });
      if (!drag) return;
      const view = wfView();
      if (drag.kind === "pinch") {
        if (pointers.size < 2) return;
        const [a, b] = Array.from(pointers.values());
        const rect = canvas.getBoundingClientRect();
        const distance = Math.hypot(a.x - b.x, a.y - b.y) || 1;
        const midX = (a.x + b.x) / 2;
        const midY = (a.y + b.y) / 2;
        const zoom = Math.min(2, Math.max(0.25, drag.zoom * (distance / drag.distance)));
        const startX = drag.midX - rect.left;
        const startY = drag.midY - rect.top;
        view.zoom = zoom;
        view.panX = (midX - rect.left) - ((startX - drag.panX) * zoom) / drag.zoom;
        view.panY = (midY - rect.top) - ((startY - drag.panY) * zoom) / drag.zoom;
        wfApplyTransform();
      } else if (drag.kind === "pan") {
        const dx = event.clientX - drag.x;
        const dy = event.clientY - drag.y;
        if (Math.abs(dx) + Math.abs(dy) > 6) {
          drag.moved = true;
          cancelPress();
        }
        view.panX = drag.panX + dx;
        view.panY = drag.panY + dy;
        wfApplyTransform();
      } else if (drag.kind === "node") {
        const dx = (event.clientX - drag.x) / view.zoom;
        const dy = (event.clientY - drag.y) / view.zoom;
        if (!drag.moved && Math.abs(dx) + Math.abs(dy) < 4) return;
        drag.moved = true;
        const node = wfNode(drag.id);
        node.x = wfSnap(drag.nodeX + dx);
        node.y = wfSnap(drag.nodeY + dy);
        drag.el.style.left = `${node.x}px`;
        drag.el.style.top = `${node.y}px`;
        wfRenderEdges();
      } else if (drag.kind === "connect" || drag.kind === "reconnect") {
        const point = wfWorldPoint(event);
        if (drag.kind === "connect") wfRenderEdges({ from: drag.start, to: point });
        else wfRenderEdges(drag.end === "to" ? { from: drag.fixed, to: point } : { from: point, to: drag.fixed }, drag.index);
        for (const el of $$(".wf-port.is-target")) el.classList.remove("is-target");
        const under = wfNodeAt(event.clientX, event.clientY);
        const selector = drag.kind === "reconnect" && drag.end === "from" ? "[data-wf-port-out]" : "[data-wf-port-in]";
        under?.querySelector(selector)?.classList.add("is-target");
      }
    });
    const finish = (event) => {
      pointers.delete(event.pointerId);
      cancelPress();
      if (!drag) return;
      if (drag.kind === "pinch") {
        if (pointers.size < 2) drag = null;
        return;
      }
      const current = drag;
      drag = null;
      canvas.classList.remove("is-panning");
      const view = wfView();
      for (const el of $$(".wf-port.is-target")) el.classList.remove("is-target");
      if (current.kind === "node") {
        if (current.moved) wfMarkDirty();
        else if (wfIsSmall() && event.type === "pointerup") {
          view.sheetOpen = true;
          wfRenderLayout();
        }
        return;
      }
      if (current.kind === "connect") {
        const under = event.type === "pointerup" ? wfNodeAt(event.clientX, event.clientY) : null;
        if (under) {
          if (wfConnect(current.from, current.output, under.dataset.wfNode)) wfMarkDirty();
          wfRenderEdges();
        } else if (event.type === "pointerup") {
          wfRenderEdges();
          const point = wfWorldPoint(event);
          wfOpenPalette({ from: current.from, output: current.output, x: point.x, y: point.y - WF_NODE_H / 2 });
        } else {
          wfRenderEdges();
        }
        return;
      }
      if (current.kind === "reconnect") {
        const conn = view.current.connections[current.index];
        const under = event.type === "pointerup" ? wfNodeAt(event.clientX, event.clientY) : null;
        if (!conn) {
          wfRenderEdges();
          return;
        }
        if (!under) {
          if (event.type === "pointerup") {
            wfDeleteEdge(current.index);
            wfSetNotice("Connection removed.", "good");
          } else {
            wfRenderEdges();
          }
          return;
        }
        const target = under.dataset.wfNode;
        if (current.end === "to") {
          if (target !== conn.to && wfCanConnect(conn.from, conn.output, target, current.index)) {
            conn.to = target;
            wfMarkDirty();
          }
        } else {
          const node = wfNode(target);
          const port = document.elementFromPoint(event.clientX, event.clientY)?.closest("[data-wf-port-out]");
          const output = port?.dataset.wfOutput || (node ? wfOutputs(node.type)[0] : "");
          if (node && output && wfCanConnect(target, output, conn.to, current.index)) {
            conn.from = target;
            conn.output = output;
            wfMarkDirty();
          }
        }
        view.selectedEdge = view.current.connections.indexOf(conn);
        wfRenderEdges();
        wfRenderPanel();
      }
    };
    canvas.addEventListener("pointerup", finish);
    canvas.addEventListener("pointercancel", finish);
    canvas.addEventListener("wheel", (event) => {
      if (!wfView().current) return;
      event.preventDefault();
      const factor = event.ctrlKey ? Math.exp(-event.deltaY / 100) : (event.deltaY < 0 ? 1.1 : 1 / 1.1);
      wfZoomAt(factor, event.clientX, event.clientY);
    }, { passive: false });
    canvas.addEventListener("dblclick", (event) => {
      if (event.target.closest("[data-wf-node]")) return;
      const point = wfWorldPoint(event);
      wfOpenPalette({ x: point.x - WF_NODE_W / 2, y: point.y - WF_NODE_H / 2 });
    });
    canvas.addEventListener("click", (event) => {
      const add = event.target.closest("[data-wf-add-after]");
      if (add) {
        const node = wfNode(add.dataset.wfAddAfter);
        if (node) wfOpenPalette({ from: node.id, output: wfOutputs(node.type)[0] });
        return;
      }
      const zoom = event.target.closest("[data-wf-zoom]");
      if (zoom) {
        if (zoom.dataset.wfZoom === "fit") wfFit();
        else if (zoom.dataset.wfZoom === "tidy") wfTidy();
        else wfZoomAt(zoom.dataset.wfZoom === "in" ? 1.2 : 1 / 1.2);
      }
    });
    canvas.addEventListener("contextmenu", (event) => {
      if (!event.target.closest("[data-wf-node]")) event.preventDefault();
    });
  }

  function wfBindPanel() {
    const panel = $("[data-wf-panel-body]");
    if (!panel) return;
    panel.addEventListener("change", async (event) => {
      const pack = event.target.closest("[data-wf-pack]");
      if (!pack) return;
      pack.disabled = true;
      const ok = await wfSetPlugin(pack.dataset.wfPack, pack.checked);
      if (!ok) pack.checked = !pack.checked;
      pack.disabled = false;
      wfRenderSide();
      wfRenderPanel();
    });
    panel.addEventListener("input", (event) => {
      const node = wfNode(wfView().selected);
      const param = event.target.closest("[data-wf-param]");
      if (node && param && !param.readOnly) {
        node.params = node.params || {};
        node.params[param.dataset.wfParam] = param.type === "checkbox" ? (param.checked ? "true" : "false") : param.value;
        wfMarkDirty();
        const el = $(`[data-wf-node="${CSS.escape(node.id)}"]`);
        if (el) {
          const title = el.querySelector(".wf-node-title");
          const sub = el.querySelector(".wf-node-sub");
          if (title) title.textContent = wfNodeTitle(node);
          if (sub) sub.textContent = node.disabled ? "Disabled" : wfNodeSummary(node);
          const link = wfShortcutLink(node);
          if ((el.querySelector("[data-wf-open-file]")?.dataset.wfOpenFile || "") !== (link ? `.forkmesh/shortcuts/${link}` : "")) {
            el.outerHTML = wfNodeHtml(node);
            window.lucide?.createIcons();
          }
        }
        if (wfIsFileMode() && wfIsRoot(node)) wfRenderToolbar();
      }
    });
    panel.addEventListener("change", (event) => {
      const view = wfView();
      const node = wfNode(view.selected);
      if (!node) return;
      const target = event.target;
      if (target.matches("[data-wf-check]")) {
        const key = target.dataset.wfCheck;
        node.params = node.params || {};
        node.params[key] = Array.from(panel.querySelectorAll(`[data-wf-check="${CSS.escape(key)}"]:checked`)).map((el) => el.value);
        wfMarkDirty();
        wfRenderCanvas();
      } else if (target.matches("[data-wf-node-name]")) {
        wfRenameNode(node, target.value);
        wfRenderCanvas();
        wfRenderPanel();
      } else if (target.matches("[data-wf-operation]")) {
        const fields = new Set((wfApp(node.type)?.operations.find((op) => op.key === target.value)?.fields || []).map((f) => f.key));
        const kept = {};
        for (const [key, value] of Object.entries(node.params || {})) if (fields.has(key)) kept[key] = value;
        node.params = { ...kept, operation: target.value };
        wfMarkDirty();
        wfRenderCanvas();
        wfRenderPanel();
      } else if (target.matches("[data-wf-credential]")) {
        node.credential = target.value;
        wfMarkDirty();
      } else if (target.matches("[data-wf-node-flag]")) {
        node[target.dataset.wfNodeFlag] = target.checked;
        wfMarkDirty();
        wfRenderCanvas();
      } else if (target.matches("select[data-wf-param]")) {
        wfRenderCanvas();
      }
    });
    panel.addEventListener("focusin", (event) => {
      const param = event.target.closest("[data-wf-param]");
      if (param) wfView().lastField = param.dataset.wfParam;
    });
    panel.addEventListener("keydown", (event) => {
      const area = event.target.closest("textarea.wf-code");
      if (!area || event.key !== "Tab" || event.shiftKey) return;
      event.preventDefault();
      const start = area.selectionStart;
      area.value = `${area.value.slice(0, start)}  ${area.value.slice(area.selectionEnd)}`;
      area.selectionStart = area.selectionEnd = start + 2;
      area.dispatchEvent(new Event("input", { bubbles: true }));
    });
    panel.addEventListener("click", async (event) => {
      const view = wfView();
      if (event.target.closest("[data-wf-delete-node]") && view.selected) {
        wfDeleteNode(view.selected);
        return;
      }
      if (event.target.closest("[data-wf-delete-edge]")) {
        wfDeleteEdge(view.selectedEdge);
        return;
      }
      const newCred = event.target.closest("[data-wf-new-credential]");
      if (newCred) {
        wfOpenCredentials(newCred.dataset.wfNewCredential);
        return;
      }
      const insert = event.target.closest("[data-wf-insert]");
      if (insert) {
        event.preventDefault();
        wfInsertExpression(insert.dataset.wfInsert);
        return;
      }
      const step = event.target.closest("[data-wf-select-node]");
      if (step && wfNode(step.dataset.wfSelectNode)) {
        wfSelect(step.dataset.wfSelectNode);
        return;
      }
      const script = event.target.closest("[data-wf-view-script]");
      if (script) {
        void wfOpenScript(script.dataset.wfViewScript || "");
        return;
      }
      if (event.target.closest("[data-wf-copy-hook]")) {
        const input = $("[data-wf-hook-url]");
        try {
          await navigator.clipboard.writeText(input?.value || "");
          wfSetNotice("Webhook URL copied.", "good");
        } catch {
          input?.select();
        }
      }
    });
    panel.addEventListener("pointerdown", (event) => {
      if (event.target.closest("[data-wf-insert]")) event.preventDefault();
    });
  }

  function wfBindDialogs() {
    const palette = $("[data-wf-palette]");
    palette?.addEventListener("click", (event) => {
      const view = wfView();
      if (event.target === palette || event.target.closest("[data-wf-palette-close]")) {
        wfClosePalette();
        return;
      }
      const cat = event.target.closest("[data-wf-cat]");
      if (cat) {
        view.paletteCategory = cat.dataset.wfCat;
        wfRenderPalette();
        return;
      }
      const pick = event.target.closest("[data-wf-pick]");
      if (pick) wfAddNode(pick.dataset.wfPick, view.paletteAfter);
    });
    const search = $("[data-wf-palette-search]");
    search?.addEventListener("input", () => {
      wfView().paletteQuery = search.value;
      wfRenderPalette();
    });
    search?.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        const first = wfPaletteEntries()[0];
        if (first) wfAddNode(first.type, wfView().paletteAfter);
      } else if (event.key === "ArrowDown") {
        event.preventDefault();
        $("[data-wf-pick]")?.focus();
      }
    });
    const creds = $("[data-wf-credentials]");
    creds?.addEventListener("click", (event) => {
      if (event.target === creds || event.target.closest("[data-wf-credentials-close]")) {
        wfCloseCredentials();
        return;
      }
      const del = event.target.closest("[data-wf-delete-credential]");
      if (del) void wfDeleteCredential(del.dataset.wfDeleteCredential);
    });
    creds?.addEventListener("change", (event) => {
      if (event.target.matches("[data-wf-credential-integration]")) {
        wfView().credentialIntegration = event.target.value;
        wfRenderCredentials();
      }
    });
    creds?.addEventListener("submit", (event) => {
      const form = event.target.closest("[data-wf-credential-form]");
      if (!form) return;
      event.preventDefault();
      void wfSaveCredential(form);
    });
    const dialog = $("[data-wf-dialog]");
    dialog?.addEventListener("click", (event) => {
      if (event.target === dialog || event.target.closest("[data-wf-dialog-close]")) {
        wfCloseDialog();
        return;
      }
      const template = event.target.closest("[data-wf-template]");
      if (template) void wfUseTemplate(template.dataset.wfTemplate);
    });
    dialog?.addEventListener("submit", (event) => {
      const form = event.target.closest("[data-wf-import-form]");
      if (!form) return;
      event.preventDefault();
      void wfImportUrl($("[data-wf-import-input]")?.value || "");
    });
  }

  function wfBindToolbar() {
    const app = $("[data-wf-app]");
    app?.addEventListener("click", (event) => {
      const view = wfView();
      const mode = event.target.closest("[data-wf-mode]");
      if (mode) {
        wfSetMode(mode.dataset.wfMode);
        return;
      }
      if (event.target.closest("[data-wf-toggle-list]")) {
        view.listOpen = !view.listOpen;
        view.menuOpen = false;
        wfRenderLayout();
        wfRenderToolbar();
        return;
      }
      if (event.target.closest("[data-wf-scrim]")) {
        wfCloseDrawers();
        return;
      }
      if (event.target.closest("[data-wf-toggle-sheet]")) {
        view.sheetOpen = !view.sheetOpen;
        wfRenderLayout();
        return;
      }
      if (event.target.closest("[data-wf-close-sheet]")) {
        view.sheetOpen = false;
        wfRenderLayout();
        return;
      }
      if (event.target.closest("[data-wf-new]")) {
        wfNewWorkflow();
        return;
      }
      if (event.target.closest("[data-wf-send-failure]")) {
        void wfSendFailure();
        return;
      }
      const open = event.target.closest("[data-wf-open]");
      if (open) {
        void wfOpenWorkflow(open.dataset.wfOpen);
        return;
      }
      const openFile = event.target.closest("[data-wf-open-file]");
      if (openFile) {
        void wfOpenFile(openFile.dataset.wfOpenFile);
        return;
      }
      if (event.target.closest("[data-wf-install-plugin]")) {
        void (async () => {
          if (await wfSetPlugin(WF_PACKAGE, true)) {
            view.pluginOff = false;
            location.reload();
          }
        })();
        return;
      }
      const shipped = event.target.closest("[data-wf-add-package-file]");
      if (shipped) {
        void wfAddPackageFile(shipped.dataset.wfAddPackageFile);
        return;
      }
      const newFile = event.target.closest("[data-wf-new-file]");
      if (newFile) {
        void wfNewFile(newFile.dataset.wfNewFile);
        return;
      }
      if (event.target.closest("[data-wf-open-credentials]")) {
        wfOpenCredentials();
        return;
      }
      if (event.target.closest("[data-wf-open-templates]")) {
        view.menuOpen = false;
        wfRenderToolbar();
        void wfOpenTemplates();
        return;
      }
      if (event.target.closest("[data-wf-menu-toggle]")) {
        view.menuOpen = !view.menuOpen;
        wfRenderToolbar();
        return;
      }
      if (view.menuOpen && !event.target.closest("[data-wf-menu]")) {
        view.menuOpen = false;
        wfRenderToolbar();
      }
      if (event.target.closest("[data-wf-export]")) {
        view.menuOpen = false;
        wfRenderToolbar();
        void wfExport();
        return;
      }
      if (event.target.closest("[data-wf-upload]")) {
        view.menuOpen = false;
        wfRenderToolbar();
        $("[data-wf-upload-input]")?.click();
        return;
      }
      if (event.target.closest("[data-wf-import-url]")) {
        view.menuOpen = false;
        wfRenderToolbar();
        wfOpenImportUrl();
        return;
      }
      if (event.target.closest("[data-wf-duplicate]")) {
        view.menuOpen = false;
        wfDuplicate();
        return;
      }
      if (view.busy || !view.current) return;
      if (event.target.closest("[data-wf-add]")) wfOpenPalette();
      else if (event.target.closest("[data-wf-save]")) void wfSave();
      else if (event.target.closest("[data-wf-run]")) void wfRun();
      else if (event.target.closest("[data-wf-preview]")) void wfPreviewFile();
      else if (event.target.closest("[data-wf-delete]")) void wfDelete();
      else if (event.target.closest("[data-wf-runs-toggle]")) {
        view.runsOpen = !view.runsOpen;
        wfRenderToolbar();
        wfRenderRuns();
        if (view.runsOpen) void wfLoadRuns();
      } else {
        const run = event.target.closest("[data-wf-run-id]");
        if (run) void wfOpenRun(run.dataset.wfRunId);
      }
    });
    $("[data-wf-repo-form]")?.addEventListener("submit", (event) => {
      event.preventDefault();
      void wfOpenRepo($("[data-wf-repo-input]")?.value || "");
    });
    const upload = $("[data-wf-upload-input]");
    upload?.addEventListener("change", async () => {
      const file = upload.files?.[0];
      upload.value = "";
      if (!file) return;
      if (file.size > 1024 * 1024) {
        wfSetNotice(WF_ERROR_TEXT.document_too_large, "bad");
        return;
      }
      await wfImportText(await file.text(), file.name);
    });
    const name = $("[data-wf-name]");
    name?.addEventListener("input", () => {
      const view = wfView();
      if (!view.current || wfIsFileMode()) return;
      view.current.name = name.value;
      wfMarkDirty();
    });
    const active = $("[data-wf-active]");
    active?.addEventListener("change", () => {
      const view = wfView();
      if (!view.current || wfIsFileMode()) return;
      view.current.active = active.checked;
      wfMarkDirty();
      wfRenderToolbar();
      void wfSave({ quiet: true }).then((ok) => {
        if (ok) wfSetNotice(view.current.active ? "Workflow is active: its webhook and schedule now run it." : "Workflow is inactive.", "good");
      });
    });
    document.addEventListener("keydown", (event) => {
      const view = wfView();
      if (!view.current) return;
      if (event.key === "Escape") {
        wfClosePalette();
        wfCloseCredentials();
        wfCloseDialog();
        wfCloseDrawers();
        wfRenderToolbar();
        return;
      }
      if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === "s") {
        event.preventDefault();
        void wfSave();
        return;
      }
      const tag = document.activeElement?.tagName || "";
      if (["INPUT", "TEXTAREA", "SELECT"].includes(tag) || document.activeElement?.isContentEditable) return;
      if (!$("[data-wf-palette]")?.hidden || !$("[data-wf-credentials]")?.hidden || !$("[data-wf-dialog]")?.hidden) return;
      if (event.key === "Delete" || event.key === "Backspace") {
        if (view.selectedEdge >= 0) {
          event.preventDefault();
          wfDeleteEdge(view.selectedEdge);
        } else if (view.selected) {
          event.preventDefault();
          wfDeleteNode(view.selected);
        }
      }
    });
    window.addEventListener("beforeunload", (event) => {
      if (!wfView().dirty) return;
      event.preventDefault();
      event.returnValue = "";
    });
    window.addEventListener("resize", () => {
      if (!wfIsSmall()) {
        const view = wfView();
        view.listOpen = false;
        view.sheetOpen = false;
        wfRenderLayout();
      }
    });
  }

  async function loadWorkflowsPage() {
    const view = wfView();
    if (!wfAccountName()) {
      view.loading = false;
      renderWorkflowsPage();
      return;
    }
    view.loading = true;
    try {
      const [catalog, list, creds] = await Promise.all([
        fetchJson("/api/workflows/catalog"),
        fetchJson("/api/workflows", { fresh: true }),
        fetchJson("/api/workflows/credentials", { fresh: true }),
      ]);
      view.catalog = catalog;
      view.workflows = Array.isArray(list.workflows) ? list.workflows : [];
      view.credentials = Array.isArray(creds.credentials) ? creds.credentials : [];
      await wfLoadPlugins();
    } catch (error) {
      view.loading = false;
      if (error?.code === "plugin_not_installed" || error?.message === "plugin_not_installed") {
        view.pluginOff = true;
        renderWorkflowsPage();
        return;
      }
      wfSetNotice(wfErrorText(error, "Workflows are unavailable right now."), "bad");
      renderWorkflowsPage();
      return;
    }
    view.loading = false;
    const params = new URLSearchParams(location.search);
    const repo = wfPluginOn(WF_FILES_PACKAGE) ? (params.get("repo") || "") : "";
    const file = params.get("file") || "";
    if (repo) {
      view.mode = "files";
      wfRenderSide();
      void wfLoadRepoOptions();
      if (await wfOpenRepo(repo, { quiet: Boolean(file) }) && file) {
        await wfOpenFile(file);
        if (wfIsFileMode()) return;
      }
    }
    const wanted = params.get("id") || "";
    const id = /^[0-9a-f]{32}$/.test(wanted) && view.workflows.some((item) => item.id === wanted)
      ? wanted
      : (view.workflows[0]?.id || "");
    if (id) {
      await wfOpenWorkflow(id);
      if (repo) {
        view.mode = "files";
        wfRenderSide();
      }
    } else {
      view.current = wfDraft();
      view.dirty = false;
      view.selected = view.current.nodes[0].id;
      if (repo) view.mode = "files";
      renderWorkflowsPage();
      wfFit();
    }
  }

  function initWorkflowsPage() {
    wfBindToolbar();
    wfBindCanvas();
    wfBindPanel();
    wfBindDialogs();
    renderWorkflowsPage();
    void loadWorkflowsPage();
  }
  return { initWorkflowsPage };
  };
})();

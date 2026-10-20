  // ---------------------------------------------------------------------------
  // Repository Actions tab.
  //
  // The runs come from a mirror node that operates an attested Actions
  // executor: the Go mirror daemon's sandboxed runner (server/actions.go), or
  // a desktop publishing its own runs read-only. This tab
  // reads that executor's bounded summary (GET actions/runs) and, when the
  // answering node advertises it, sends control requests back to the same node
  // (POST actions/control). Wire contract:
  // www/docs/operations/mirror-actions-control.md.
  //
  // The workflow list needs no session: it is read from .forkmesh/ in the
  // repository tree like any other file. Who sees runs, logs and controls:
  // docs/server/instance-actions.md.
  // ---------------------------------------------------------------------------

  const REPO_ACTIONS_WORKFLOW_PATH = /^\.forkmesh\/[A-Za-z0-9._-]{1,96}\.ya?ml$/;
  const REPO_ACTIONS_STEP_STATES = {
    pending: { icon: "circle", tone: "text-muted-foreground", label: "pending" },
    running: { icon: "loader-circle", tone: "text-attention", label: "running" },
    success: { icon: "check-circle-2", tone: "text-success", label: "passed" },
    failed: { icon: "x-circle", tone: "text-destructive", label: "failed" },
    skipped: { icon: "circle-slash", tone: "text-muted-foreground", label: "skipped" },
    cancelled: { icon: "circle-slash", tone: "text-muted-foreground", label: "cancelled" },
  };
  // What the executor answers when it turns a control request down, in the
  // words of someone looking at the button they just pressed.
  const REPO_ACTIONS_CONTROL_REASONS = {
    repository_not_found: "the executor does not mirror this repository",
    actions_disabled: "Actions are turned off for this repository on that node",
    operator_disabled: "the node's operator has turned its Actions executor off",
    ref_not_found: "that branch is not on the mirror yet",
    workflow_not_found: "the workflow file is not on that branch",
    workflow_invalid: "the workflow file does not parse",
    workflow_disabled: "the workflow is disabled",
    not_manual: "the workflow has no workflow_dispatch trigger",
    wrong_node: "the workflow is pinned to a different node",
    run_not_found: "that run is no longer on the executor",
    not_cancellable: "the run already finished",
    not_awaiting_approval: "the run is not waiting for approval",
    invalid_request: "the executor refused the request",
    run_active: "a run of this workflow is already queued or running",
    input_required: "a required input is empty",
    input_invalid: "an input value is not allowed",
    forbidden: "only the repository owner and organization writers can do that",
  };
  const REPO_ACTIONS_EVENTS = ["push", "schedule", "workflow_dispatch", "issues", "issue_comment"];
  const REPO_ACTIONS_INPUT_NAME = /^[A-Za-z_][A-Za-z0-9_-]{0,63}$/;
  const ACTIONS_SECRET_NAME = /^[A-Z_][A-Z0-9_]{0,63}$/;
  const ACTIONS_SECRET_MAX_BYTES = 48 * 1024;

  function repoActionsView() {
    if (!state.repoActionsView) {
      state.repoActionsView = {
        key: "", phase: "idle", error: "", node: "", updatedAt: 0,
        schemaVersion: 1, controlAvailable: false, actionsEnabled: null, access: "",
        runs: [], controls: [], summaryWorkflows: null, treeWorkflows: null,
        selectedWorkflow: "", selectedRunId: 0, pending: [], notice: null,
        timer: 0, loading: false, branchesAsked: false, logs: {},
      };
    }
    return state.repoActionsView;
  }

  function repoActionsIsCurrent(repo) {
    return Boolean(repo)
      && state.activeRepoTab === "actions"
      && state.selectedRepo
      && repoKey(state.selectedRepo) === repoKey(repo);
  }

  function stopRepoActionsRefresh() {
    const view = state.repoActionsView;
    if (view?.timer) {
      window.clearTimeout(view.timer);
      view.timer = 0;
    }
  }

  document.addEventListener("visibilitychange", () => {
    const repo = state.selectedRepo;
    const view = state.repoActionsView;
    if (document.hidden || !view || view.timer || !repoActionsIsCurrent(repo)) return;
    void loadRepoActions(repo, { background: true });
  });

  function repoActionsRunIsLive(run) {
    return ["running", "queued", "awaiting-approval"].includes(run?.status);
  }

  // Poll only while this tab is on screen. A live run or an unanswered control
  // earns a short interval; an idle page backs off, and a hidden idle page
  // stops asking altogether (every read is a relay -> mirror round trip).
  function scheduleRepoActionsRefresh(repo) {
    stopRepoActionsRefresh();
    if (!repoActionsIsCurrent(repo)) return;
    const view = repoActionsView();
    if (view.phase !== "ready" && view.phase !== "unavailable") return;
    const active = view.pending.length > 0 || view.runs.some(repoActionsRunIsLive);
    if (!active && document.hidden) return;
    const delay = active
      ? (document.hidden ? 30000 : 6000)
      : 60000;
    view.timer = window.setTimeout(() => {
      view.timer = 0;
      void loadRepoActions(repo, { background: true });
    }, delay);
  }

  function repoActionsCleanLine(value, limit) {
    return String(value ?? "")
      .replace(/[\u0000-\u001f\u007f]/g, " ")
      .replace(/\s+/g, " ")
      .trim()
      .slice(0, limit);
  }

  function normalizeRepoActionsWorkflow(entry) {
    const path = String(entry?.path || "");
    if (!REPO_ACTIONS_WORKFLOW_PATH.test(path)) return null;
    const list = (value, limit) => (Array.isArray(value) ? value : [])
      .map((item) => repoActionsCleanLine(item, limit))
      .filter(Boolean)
      .slice(0, 8);
    const events = list(entry?.events, 40);
    return {
      path,
      name: repoActionsCleanLine(entry?.name, 160) || path.split("/").pop(),
      events,
      runsOn: list(entry?.runsOn, 64),
      manual: entry?.manual === true || events.includes("workflow_dispatch"),
      enabled: entry?.enabled !== false,
      valid: entry?.valid !== false,
      inputs: normalizeRepoActionsInputs(entry?.inputs),
    };
  }

  function normalizeRepoActionsInputs(raw) {
    const entries = Array.isArray(raw)
      ? raw.map((item) => [item?.name, item])
      : raw && typeof raw === "object" ? Object.entries(raw) : [];
    const inputs = [];
    for (const [rawName, spec] of entries) {
      const name = String(rawName || "");
      if (!REPO_ACTIONS_INPUT_NAME.test(name) || inputs.some((input) => input.name === name)) continue;
      const details = spec && typeof spec === "object" ? spec : {};
      const kind = ["string", "choice", "boolean"].includes(details.type) ? details.type : "string";
      const options = (Array.isArray(details.options) ? details.options : [])
        .map((option) => repoActionsCleanLine(option, 120))
        .filter(Boolean)
        .slice(0, 50);
      const fallback = details.default;
      inputs.push({
        name,
        description: repoActionsCleanLine(details.description, 200),
        required: details.required === true || details.required === "true",
        type: kind === "choice" && !options.length ? "string" : kind,
        options,
        defaultValue: kind === "boolean"
          ? fallback === true || String(fallback).toLowerCase() === "true"
          : repoActionsCleanLine(fallback ?? "", 1000),
      });
      if (inputs.length >= 20) break;
    }
    return inputs;
  }

  function repoActionsRunEvent(value) {
    const event = String(value || "").toLowerCase();
    return REPO_ACTIONS_EVENTS.includes(event) ? event : "";
  }

  function normalizeRepoActionsRun(run) {
    const id = Number(run?.id);
    const status = String(run?.status || "").toLowerCase();
    const workflow = repoActionsCleanLine(run?.workflow, 160);
    if (!Number.isSafeInteger(id) || id <= 0 || !workflow || !PULL_CHECK_STATES[status]) {
      return null;
    }
    const workflowPath = String(run?.workflowPath || "");
    return {
      id,
      status,
      workflow,
      workflowPath: REPO_ACTIONS_WORKFLOW_PATH.test(workflowPath) ? workflowPath : "",
      event: repoActionsRunEvent(run?.event),
      commit: immutableGitCommit(run?.commit) || "",
      ref: repoActionsCleanLine(run?.ref, 220),
      actor: repoActionsCleanLine(run?.actor, 64),
      pullNumber: Math.max(0, Math.floor(Number(run?.pullNumber) || 0)),
      createdAt: Math.max(0, Number(run?.createdAt) || 0),
      startedAt: Math.max(0, Number(run?.startedAt) || 0),
      finishedAt: Math.max(0, Number(run?.finishedAt) || 0),
      steps: (Array.isArray(run?.steps) ? run.steps : []).slice(0, 64)
        .map((step) => ({
          name: repoActionsCleanLine(step?.name, 120),
          status: REPO_ACTIONS_STEP_STATES[step?.status] ? step.status : "pending",
        }))
        .filter((step) => step.name),
      logTail: String(run?.logTail || "").replace(/\u0000/g, ""),
    };
  }

  function normalizeRepoActionsControl(entry) {
    const requestId = String(entry?.requestId || "");
    if (!/^[A-Za-z0-9_-]{16,64}$/.test(requestId)) return null;
    return {
      requestId,
      action: repoActionsCleanLine(entry?.action, 40),
      status: entry?.status === "applied" ? "applied" : "rejected",
      reason: repoActionsCleanLine(entry?.reason, 40),
      runId: Math.max(0, Math.floor(Number(entry?.runId) || 0)),
      finishedAt: Math.max(0, Number(entry?.finishedAt) || 0),
    };
  }

  // A workflow file read straight from the tree, for visitors without access
  // to an executor summary and for executors that still publish summary v1.
  // This is a label reader, not a YAML parser: it only lifts the top-level
  // `name:` and the event names under `on:`.
  function parseRepoActionsWorkflowText(path, text) {
    const lines = String(text || "").split(/\r?\n/);
    let name = "";
    const events = [];
    for (let index = 0; index < lines.length; index += 1) {
      const line = lines[index];
      const nameMatch = /^name\s*:\s*(.+?)\s*$/.exec(line);
      if (nameMatch && !name) {
        name = nameMatch[1].replace(/^["']|["']$/g, "");
        continue;
      }
      const onMatch = /^(?:on|"on"|'on')\s*:\s*(.*)$/.exec(line);
      if (!onMatch) continue;
      const inline = onMatch[1].replace(/#.*$/, "").trim();
      if (inline) {
        inline.replace(/^\[|\]$/g, "").split(",")
          .forEach((item) => events.push(item.trim().replace(/^["']|["']$/g, "")));
        continue;
      }
      // Block form: the event names are the keys (or list items) at the first
      // child indentation; anything deeper is that event's own filter.
      let childIndent = -1;
      for (let next = index + 1; next < lines.length; next += 1) {
        const child = lines[next];
        if (!child.trim() || /^\s*#/.test(child)) continue;
        const indent = child.length - child.trimStart().length;
        if (indent === 0) break;
        if (childIndent < 0) childIndent = indent;
        if (indent !== childIndent) continue;
        const item = /^\s+(?:-\s*)?["']?([A-Za-z_]+)["']?\s*:?/.exec(child);
        if (item) events.push(item[1]);
      }
    }
    const unique = Array.from(new Set(events.filter(Boolean)));
    return normalizeRepoActionsWorkflow({
      path, name, events: unique,
      manual: unique.includes("workflow_dispatch"),
      enabled: true, valid: true,
    });
  }

  async function loadRepoActionsTreeWorkflows(repo) {
    const view = repoActionsView();
    try {
      const ref = repoDefaultBranch(repo);
      const tree = await fetchRepoJson(repoLiveUrl(repo, "tree", { path: ".forkmesh", ref }));
      const paths = (Array.isArray(tree?.entries) ? tree.entries : [])
        .filter((entry) => entry?.type !== "tree")
        .map((entry) => `.forkmesh/${String(entry?.name || "")}`)
        .filter((path) => REPO_ACTIONS_WORKFLOW_PATH.test(path))
        .sort()
        .slice(0, 32);
      const blobs = paths.length ? await fetchRepoBlobs(repo, paths, { ref }) : {};
      if (!repoActionsIsCurrent(repo)) return;
      view.treeWorkflows = paths
        .map((path) => parseRepoActionsWorkflowText(path, blobs[path] ? blobText(blobs[path]) : ""))
        .filter(Boolean);
    } catch (_) {
      if (!repoActionsIsCurrent(repo)) return;
      view.treeWorkflows = view.treeWorkflows || [];
    }
    renderRepoActions(repo);
  }

  function repoActionsWorkflows(view) {
    return view.summaryWorkflows || view.treeWorkflows || [];
  }

  async function loadRepoActions(repo, { background = false } = {}) {
    if (!repo) return;
    const view = repoActionsView();
    const key = repoKey(repo);
    if (view.key !== key) {
      stopRepoActionsRefresh();
      Object.assign(view, {
        key, phase: "idle", error: "", node: "", local: false, host: "", source: "", access: "", updatedAt: 0, schemaVersion: 1,
        controlAvailable: false, actionsEnabled: null, runs: [], controls: [],
        summaryWorkflows: null, treeWorkflows: null, selectedWorkflow: "",
        selectedRunId: 0, pending: [], notice: null, loading: false,
        branchesAsked: false, logs: {},
      });
    }
    const initialRun = state.repoActionsInitialRun;
    if (initialRun && initialRun.key === key) {
      view.selectedRunId = initialRun.runId;
      state.repoActionsInitialRun = null;
    }
    if (view.loading) return;
    if (view.treeWorkflows === null) void loadRepoActionsTreeWorkflows(repo);
    if (!state.session?.sessionToken && !dashboardOnInstance()) {
      view.phase = "signed-out";
      renderRepoActions(repo);
      return;
    }
    view.loading = true;
    if (!background) {
      view.phase = view.phase === "ready" ? "ready" : "loading";
      renderRepoActions(repo);
    }
    let payload = null;
    let failure = null;
    try {
      payload = await orgAgentRequest("GET", `${repoApiBase(repo)}/actions/runs`);
    } catch (error) {
      failure = error;
    }
    view.loading = false;
    if (!repoActionsIsCurrent(repo) || view.key !== key) return;
    if (payload?.ok === true) {
      view.phase = "ready";
      view.error = "";
      view.node = repoActionsCleanLine(payload.node, 80);
      view.source = payload.source === "runner" ? "runner" : "mirror";
      view.local = view.source === "runner" && payload.local === true;
      view.host = view.local ? repoActionsCleanLine(payload.host, 253) : "";
      view.access = view.source === "runner" && payload.access === "public" ? "public" : "member";
      view.updatedAt = Math.max(0, Number(payload.updatedAt) || 0);
      view.schemaVersion = Number(payload.schemaVersion) === 2 ? 2 : 1;
      const runner = view.source === "runner";
      view.controlAvailable = payload.controlAvailable === true && (view.schemaVersion === 2 || runner);
      view.actionsEnabled = view.schemaVersion === 2 && !runner ? payload.actionsEnabled === true : null;
      view.runs = (Array.isArray(payload.runs) ? payload.runs : [])
        .map(normalizeRepoActionsRun).filter(Boolean)
        .sort((left, right) => (right.createdAt - left.createdAt) || (right.id - left.id));
      view.controls = (Array.isArray(payload.controls) ? payload.controls : [])
        .map(normalizeRepoActionsControl).filter(Boolean);
      view.summaryWorkflows = view.schemaVersion === 2 || (runner && Array.isArray(payload.workflows))
        ? (Array.isArray(payload.workflows) ? payload.workflows : [])
          .map(normalizeRepoActionsWorkflow).filter(Boolean)
        : null;
      settleRepoActionsPending(view);
      if (runner && view.access !== "public" && view.selectedRunId) void loadRepoActionsRunLog(repo, view.selectedRunId);
      // The "Run workflow" branch picker needs the real branch list; until a
      // listing is loaded repoBranchList() only knows the selected branch.
      if (view.controlAvailable && !state.repoBranches[key]?.loaded && !view.branchesAsked) {
        view.branchesAsked = true;
        fetchRepoBranches(repo)
          .then(() => { if (view.key === key) renderRepoActions(repo); })
          .catch(() => {});
      }
    } else if (failure?.payload?.state === "unavailable") {
      // orgAgentRequest throws on any {"ok": false} body, including the quiet
      // 200 the relay sends when no attested executor answered.
      view.phase = "unavailable";
      view.error = "";
      view.runs = [];
      view.controlAvailable = false;
    } else if (failure) {
      const status = Number(failure?.status) || 0;
      // A background miss keeps the last good picture on screen; the next
      // tick asks again.
      if (!(background && view.phase === "ready" && status !== 401 && status !== 403)) {
        view.phase = status === 403 ? "forbidden"
          : status === 401 ? "signed-out"
            : status === 404 ? "unavailable" : "error";
        view.error = status === 403 || status === 401 || status === 404
          ? "" : "The relay could not reach this repository's mirrors.";
      }
    } else {
      view.phase = "error";
      view.error = "The relay answered with something this page does not understand.";
    }
    renderRepoActions(repo);
    scheduleRepoActionsRefresh(repo);
  }

  // A control is "pending" from the 202 until the executor lists its request
  // id among the summary's controls. Executors are polled, so an answer that
  // has not arrived in ninety seconds is reported as such rather than spun on.
  function settleRepoActionsPending(view) {
    if (!view.pending.length) return;
    const byId = new Map(view.controls.map((entry) => [entry.requestId, entry]));
    const now = Date.now();
    view.pending = view.pending.filter((pending) => {
      const outcome = byId.get(pending.requestId);
      if (outcome) {
        const reason = REPO_ACTIONS_CONTROL_REASONS[outcome.reason]
          || (outcome.reason ? outcome.reason.replace(/_/g, " ") : "");
        view.notice = outcome.status === "applied"
          ? { tone: "good", text: `${pending.label}: done.` }
          : { tone: "bad", text: `${pending.label} was refused: ${reason || "no reason given"}.` };
        if (outcome.status === "applied" && outcome.runId && pending.action === "dispatch") {
          view.selectedRunId = outcome.runId;
        }
        return false;
      }
      if (now - pending.sentAt > 90000) {
        view.notice = {
          tone: "muted",
          text: `${pending.label}: the executor on ${view.node || "the mirror"} has not answered yet.`,
        };
        return false;
      }
      return true;
    });
  }

  function repoActionsRequestId() {
    const bytes = new Uint8Array(18);
    window.crypto.getRandomValues(bytes);
    return bytesToB64url(bytes);
  }

  async function sendRepoActionsRunnerControl(repo, action, fields, label) {
    const view = repoActionsView();
    const key = view.key;
    view.notice = null;
    view.pending.push({ requestId: "", action, label, sentAt: Date.now(), ...fields });
    renderRepoActions(repo);
    let answer = null;
    let failure = null;
    try {
      answer = await orgAgentRequest("POST", `${repoApiBase(repo)}/actions/control`, { action, ...fields });
    } catch (error) {
      failure = error;
    }
    view.pending = view.pending.filter((entry) => !(entry.action === action && entry.label === label));
    if (view.key !== key) return;
    if (failure) {
      const status = Number(failure?.status) || 0;
      const code = String(failure?.code || failure?.payload?.error || "");
      const reason = REPO_ACTIONS_CONTROL_REASONS[code] || failure?.payload?.message || "";
      view.notice = {
        tone: "bad",
        text: status === 403
          ? `${label}: only the repository owner and organization writers can do that.`
          : `${label} was refused${reason ? `: ${reason}` : "."}`,
      };
    } else {
      const runId = Math.max(0, Math.floor(Number(answer?.runId) || 0));
      view.notice = { tone: "good", text: `${label}: done.` };
      if (runId && (action === "dispatch" || action === "rerun")) view.selectedRunId = runId;
    }
    if (!repoActionsIsCurrent(repo)) return;
    renderRepoActions(repo);
    void loadRepoActions(repo, { background: true });
  }

  async function loadRepoActionsRunLog(repo, runId) {
    const view = repoActionsView();
    const key = view.key;
    const run = view.runs.find((entry) => entry.id === runId);
    if (!run || view.source !== "runner" || view.access === "public") return;
    const cached = view.logs[runId];
    if (cached?.loading) return;
    if (cached && cached.done && !repoActionsRunIsLive(run)) return;
    view.logs[runId] = { ...(cached || { text: "" }), loading: true };
    let text = null;
    try {
      const payload = await orgAgentRequest("GET", `${repoApiBase(repo)}/actions/runs/${runId}/log`);
      const value = typeof payload?.log === "string" ? payload.log
        : typeof payload?.data === "string" ? payload.data : null;
      if (value !== null) text = value.replace(/\u0000/g, "");
    } catch (_) {
      text = null;
    }
    if (view.key !== key) return;
    view.logs[runId] = {
      text: text ?? cached?.text ?? "",
      full: text !== null || Boolean(cached?.full),
      done: text !== null,
      loading: false,
    };
    if (repoActionsIsCurrent(repo) && view.selectedRunId === runId) renderRepoActions(repo);
  }

  async function sendRepoActionsControl(repo, action, fields, label) {
    const view = repoActionsView();
    if (repo && view.controlAvailable && view.source === "runner") {
      await sendRepoActionsRunnerControl(repo, action, fields, label);
      return;
    }
    if (!repo || !view.controlAvailable || !view.node) return;
    const requestId = repoActionsRequestId();
    view.notice = null;
    view.pending.push({ requestId, action, label, sentAt: Date.now(), ...fields });
    renderRepoActions(repo);
    try {
      await orgAgentRequest("POST", `${repoApiBase(repo)}/actions/control`, {
        requestId, action, node: view.node, ...fields,
      });
    } catch (error) {
      view.pending = view.pending.filter((entry) => entry.requestId !== requestId);
      const status = Number(error?.status) || 0;
      view.notice = {
        tone: "bad",
        text: status === 403
          ? `${label}: only the repository owner and organization writers can do that.`
          : status === 503
            ? `${label}: ${view.node} is not reachable right now.`
            : `${label} could not be sent.`,
      };
      renderRepoActions(repo);
      return;
    }
    if (!repoActionsIsCurrent(repo)) return;
    renderRepoActions(repo);
    // Ask once soon after the executor's 4 s spool tick, then fall back to
    // the normal cadence.
    stopRepoActionsRefresh();
    view.timer = window.setTimeout(() => {
      view.timer = 0;
      void loadRepoActions(repo, { background: true });
    }, 5000);
  }

  function repoActionsDuration(run) {
    if (!run.startedAt) return "";
    const end = run.finishedAt || (run.status === "running" ? Date.now() : 0);
    if (!end || end < run.startedAt) return "";
    const seconds = Math.round((end - run.startedAt) / 1000);
    if (seconds < 60) return `${seconds}s`;
    const minutes = Math.floor(seconds / 60);
    if (minutes < 60) return `${minutes}m ${seconds % 60}s`;
    return `${Math.floor(minutes / 60)}h ${minutes % 60}m`;
  }

  function repoActionsRefLabel(ref) {
    return String(ref || "").replace(/^refs\/heads\//, "").replace(/^refs\/tags\//, "");
  }

  function repoActionsRunMatchesWorkflow(run, workflow) {
    if (!workflow) return true;
    return run.workflowPath ? run.workflowPath === workflow.path : run.workflow === workflow.name;
  }

  function renderRepoActionsExecutor(repo, view) {
    const mirrors = (Array.isArray(state.repoMirrors) ? state.repoMirrors : [])
      .filter((mirror) => mirror && mirror.actionsEnabled === true);
    const others = mirrors
      .map((mirror) => repoActionsCleanLine(mirror.node, 80))
      .filter((node) => node && node !== view.node);
    const busy = view.runs.some((run) => run.status === "running");
    const canToggle = view.controlAvailable && view.actionsEnabled !== null;
    const toggling = view.pending.some((entry) => entry.action === "set-repository-enabled");
    const stateLabel = view.actionsEnabled === false
      ? "paused for this repository"
      : busy ? "running" : "idle";
    const stateTone = view.actionsEnabled === false
      ? "fm-label-attention" : busy ? "fm-label-attention" : "fm-label-success";
    return `
      <div class="flex flex-wrap items-center justify-between gap-3 border-b border-border px-4 py-3">
        <div class="min-w-0">
          <div class="flex flex-wrap items-center gap-2 text-xs text-foreground"><i data-lucide="server" class="h-3.5 w-3.5 text-muted-foreground"></i>${view.local ? `<span>Runs on this server</span>${view.host ? `<span class="font-mono font-semibold">${escapeHtml(view.host)}</span>` : ""}` : `<span>${view.source === "runner" ? "Runs on runner" : "Runs on mirror"}</span>${view.node ? `<span class="font-mono font-semibold">${escapeHtml(view.node)}</span>` : ""}`}<span class="fm-label ${stateTone}">${escapeHtml(stateLabel)}</span></div>
          <p class="mt-1 text-[11px] leading-4 text-muted-foreground">${view.updatedAt ? `Executor state from ${escapeHtml(relativeTimeLabel(view.updatedAt))}.` : ""}${others.length ? ` Also enabled on ${escapeHtml(others.join(", "))}.` : ""}${view.access === "public" ? ` ${repoActionsPublicNote()}` : view.source === "runner" && view.controlAvailable ? " Pushes, schedules, issue events and manual runs run here. Pick a workflow to run it by hand." : view.source === "runner" ? " A push to the default branch runs here while the owner's desktop is away; desktop runs show in the desktop app and on paired phones." : view.controlAvailable ? "" : " This node publishes runs read-only; controls appear once its operator enables the actions-control operation."}</p>
        </div>
        <div class="flex shrink-0 items-center gap-2">
          ${canToggle ? `<button type="button" data-repo-actions-repository-enabled="${view.actionsEnabled ? "false" : "true"}" ${toggling ? "disabled" : ""} class="fm-btn fm-btn-sm"><i data-lucide="${view.actionsEnabled ? "pause" : "play"}" class="h-3.5 w-3.5"></i>${view.actionsEnabled ? "Pause Actions on this mirror" : "Run Actions on this mirror"}</button>` : ""}
          <button type="button" data-repo-actions-refresh class="fm-btn fm-btn-sm"><i data-lucide="refresh-cw" class="h-3.5 w-3.5"></i>Refresh</button>
        </div>
      </div>`;
  }

  function renderRepoActionsWorkflowList(view) {
    const workflows = repoActionsWorkflows(view);
    const selected = view.selectedWorkflow;
    const row = (path, label, detail, extra = "") => `
      <button type="button" data-repo-actions-workflow="${escapeHtml(path)}" aria-pressed="${selected === path ? "true" : "false"}" class="flex w-full min-w-0 flex-col gap-0.5 rounded-md px-2.5 py-2 text-left text-xs ${selected === path ? "bg-secondary text-foreground" : "text-muted-foreground hover:bg-secondary/60 hover:text-foreground"}">
        <span class="flex min-w-0 items-center gap-2"><span class="min-w-0 truncate font-medium">${escapeHtml(label)}</span>${extra}</span>
        ${detail ? `<span class="min-w-0 truncate font-mono text-[10px] text-muted-foreground">${escapeHtml(detail)}</span>` : ""}
      </button>`;
    if (view.treeWorkflows === null && !view.summaryWorkflows) {
      return `<p class="px-2.5 py-2 text-xs text-muted-foreground">Reading .forkmesh/ ...</p>`;
    }
    if (!workflows.length) {
      return `<p class="px-2.5 py-2 text-xs leading-5 text-muted-foreground">No workflows yet. Add a <span class="font-mono">.forkmesh/&lt;name&gt;.yml</span> file with <span class="font-mono">on:</span> and <span class="font-mono">steps:</span> to the default branch.</p>`;
    }
    return row("", "All workflows", "")
      + workflows.map((workflow) => {
        const tags = [
          workflow.enabled ? "" : `<span class="fm-label fm-label-attention">disabled</span>`,
          workflow.valid ? "" : `<span class="fm-label fm-label-danger">invalid</span>`,
        ].join("");
        const detail = [
          workflow.events.length ? `on: ${workflow.events.join(", ")}` : "",
          workflow.runsOn.length ? `runs-on: ${workflow.runsOn.join(", ")}` : "",
        ].filter(Boolean).join(" · ");
        return row(workflow.path, workflow.name, detail, tags);
      }).join("");
  }

  function renderRepoActionsWorkflowBar(repo, view, workflow) {
    if (!workflow) return "";
    const busy = (action) => view.pending.some(
      (entry) => entry.action === action && entry.workflow === workflow.path);
    const branches = repoBranchList(repo).map((branch) => branch.name).filter(Boolean);
    const preferred = repoDefaultBranch(repo);
    const inputs = view.source === "runner" ? workflow.inputs || [] : [];
    const dispatch = view.controlAvailable && workflow.manual && workflow.enabled && workflow.valid
      ? `<form data-repo-actions-dispatch="${escapeHtml(workflow.path)}" class="flex min-w-0 flex-wrap items-center gap-2">
          <label class="text-[11px] text-muted-foreground" for="repo-actions-dispatch-ref">Branch</label>
          <select id="repo-actions-dispatch-ref" data-repo-actions-dispatch-ref class="h-control-sm max-w-48 rounded-md border border-border bg-background px-2 font-mono text-control text-foreground">${branches.map((name) => `<option value="${escapeHtml(name)}"${name === preferred ? " selected" : ""}>${escapeHtml(name)}</option>`).join("")}</select>
          ${inputs.length ? `<div data-repo-actions-inputs class="grid w-full gap-2 sm:grid-cols-2">${inputs.map(renderRepoActionsInputField).join("")}</div>` : ""}
          <button type="submit" ${busy("dispatch") ? "disabled" : ""} class="fm-btn fm-btn-sm fm-btn-cta"><i data-lucide="play" class="h-3.5 w-3.5"></i>Run workflow</button>
        </form>`
      : `<span class="text-[11px] text-muted-foreground">${workflow.manual
        ? (view.access === "public" ? "Repository writers can run this workflow by hand." : view.controlAvailable ? (workflow.enabled ? "" : "Enable this workflow to run it by hand.") : "Manual runs need a mirror with the actions-control operation.")
        : "No workflow_dispatch trigger, so this workflow only runs on its events."}</span>`;
    const toggle = view.controlAvailable && view.summaryWorkflows && view.source !== "runner"
      ? `<button type="button" data-repo-actions-workflow-enabled="${workflow.enabled ? "false" : "true"}" data-repo-actions-workflow-path="${escapeHtml(workflow.path)}" ${busy("set-workflow-enabled") ? "disabled" : ""} class="fm-btn fm-btn-sm shrink-0">${workflow.enabled ? "Disable workflow" : "Enable workflow"}</button>`
      : "";
    return `
      <div class="flex flex-wrap items-center justify-between gap-3 border-b border-border bg-secondary/30 px-4 py-2.5">
        <div class="min-w-0"><span class="block truncate text-xs font-semibold text-foreground">${escapeHtml(workflow.name)}</span><a href="${escapeHtml(repoLinkUrl(repo, "blob", workflow.path))}" class="font-mono text-[10px] text-muted-foreground hover:text-foreground hover:underline">${escapeHtml(workflow.path)}</a></div>
        <div class="flex min-w-0 flex-wrap items-center gap-2">${dispatch}${toggle}</div>
      </div>`;
  }

  function renderRepoActionsInputField(input) {
    const id = `repo-actions-input-${input.name}`;
    const label = `<span class="font-mono">${escapeHtml(input.name)}</span>${input.required ? ' <span class="text-destructive">*</span>' : ""}`;
    const hint = input.description ? `<span class="text-[10px] font-normal text-muted-foreground">${escapeHtml(input.description)}</span>` : "";
    const data = `data-repo-actions-input="${escapeHtml(input.name)}" data-repo-actions-input-type="${input.type}"${input.required ? " data-repo-actions-input-required" : ""}`;
    if (input.type === "boolean") {
      return `<label for="${escapeHtml(id)}" class="inline-flex min-w-0 items-start gap-2 text-[11px] text-foreground"><input id="${escapeHtml(id)}" type="checkbox" ${data}${input.defaultValue ? " checked" : ""} class="mt-0.5 h-3.5 w-3.5 accent-primary" /><span class="grid gap-0.5">${label}${hint}</span></label>`;
    }
    const control = input.type === "choice"
      ? `<select id="${escapeHtml(id)}" ${data} class="h-control-sm w-full rounded-md border border-border bg-background px-2 text-control text-foreground">${input.required ? "" : '<option value=""></option>'}${input.options.map((option) => `<option value="${escapeHtml(option)}"${option === input.defaultValue ? " selected" : ""}>${escapeHtml(option)}</option>`).join("")}</select>`
      : `<input id="${escapeHtml(id)}" type="text" autocomplete="off" spellcheck="false" maxlength="1000" ${data}${input.required ? " required" : ""} value="${escapeHtml(input.defaultValue)}" class="h-control-sm w-full rounded-md border border-border bg-background px-2 text-control text-foreground" />`;
    return `<label for="${escapeHtml(id)}" class="grid min-w-0 gap-1 text-[11px] text-foreground">${label}${hint}${control}</label>`;
  }

  function renderRepoActionsRunControls(view, run) {
    if (!view.controlAvailable) return "";
    const busy = view.pending.some((entry) => entry.runId === run.id);
    const button = (action, label, icon) => `<button type="button" data-repo-actions-run-control="${action}" data-repo-actions-run-id="${run.id}" ${busy ? "disabled" : ""} class="fm-btn fm-btn-sm"><i data-lucide="${icon}" class="h-3.5 w-3.5"></i>${label}</button>`;
    if (run.status === "awaiting-approval" && view.source !== "runner") {
      return button("approve", "Approve and run", "shield-check") + button("reject", "Reject", "shield-x");
    }
    if (run.status === "running" || run.status === "queued") {
      return button("cancel", run.status === "running" ? "Stop run" : "Cancel", "square");
    }
    return button("rerun", "Re-run", "rotate-ccw");
  }

  function repoActionsSignInHref() {
    return `/login?next=${encodeURIComponent(location.pathname + location.search)}`;
  }

  function repoActionsPublicNote() {
    return state.session?.sessionToken
      ? "Logs and controls are shown to the repository owner and organization writers."
      : "Sign in as a repository writer to see logs and controls.";
  }

  function renderRepoActionsPublicDetail(run) {
    const hint = state.session?.sessionToken
      ? `<span>Logs are shown to the repository owner and organization writers.</span>`
      : `<span><a href="${escapeHtml(repoActionsSignInHref())}" data-repo-actions-link data-repo-actions-sign-in class="font-medium text-accent hover:underline">Sign in to see logs</a></span>`;
    const facts = [
      run.actor ? `Triggered by ${userNameHtml(run.actor)}` : "",
      run.event ? `on ${escapeHtml(run.event)}` : "",
      run.startedAt ? `started ${escapeHtml(relativeTimeLabel(run.startedAt))}` : "",
    ].filter(Boolean).join(" ");
    return `
      <div data-repo-actions-run-detail="${run.id}" data-repo-actions-public-detail class="border-t border-border bg-background">
        ${facts ? `<p class="border-b border-border px-4 py-2 text-xs text-muted-foreground">${facts}.</p>` : ""}
        <p class="flex items-center gap-2 px-4 py-3 text-xs text-muted-foreground"><i data-lucide="lock" class="h-3.5 w-3.5 shrink-0"></i>${hint}</p>
      </div>`;
  }

  function renderRepoActionsRunDetail(repo, view, run) {
    if (view.access === "public") return renderRepoActionsPublicDetail(run);
    const steps = run.steps.length
      ? `<ol class="grid gap-1 border-b border-border px-4 py-3">${run.steps.map((step) => {
        const meta = REPO_ACTIONS_STEP_STATES[step.status];
        return `<li class="flex min-w-0 items-center gap-2 text-xs text-foreground"><i data-lucide="${meta.icon}" class="h-3.5 w-3.5 shrink-0 ${meta.tone}${step.status === "running" ? " animate-spin" : ""}"></i><span class="min-w-0 truncate">${escapeHtml(step.name)}</span><span class="ml-auto shrink-0 text-[10px] ${meta.tone}">${escapeHtml(meta.label)}</span></li>`;
      }).join("")}</ol>`
      : "";
    const approval = run.status === "awaiting-approval"
      ? `<p class="fm-flash fm-flash-warning fm-flash-full border-t-0 px-4 py-2 text-xs"><i data-lucide="shield-alert"></i><span class="fm-flash-body">This run is waiting for approval because it would execute code the executor has not run for this repository before${run.pullNumber ? ` (pull request #${run.pullNumber})` : ""}. Review the commit before approving.</span></p>`
      : "";
    const fullLog = view.source === "runner" ? view.logs?.[run.id] : null;
    const logText = fullLog?.full && fullLog.text ? fullLog.text : run.logTail;
    const logNote = fullLog?.full && fullLog.text
      ? "Full redacted log (up to 1 MiB)."
      : view.source === "runner"
        ? (fullLog?.loading ? "Loading the full log ..." : "Last lines of the redacted log.")
        : `Last 16 KiB of the redacted log. The full log stays on ${view.local ? (view.host || "this server") : view.node}.`;
    const log = logText
      ? `<pre data-repo-actions-log class="max-h-[28rem] overflow-auto whitespace-pre-wrap break-words px-4 py-3 font-mono text-[11px] leading-[1.15rem] text-muted-foreground">${escapeHtml(logText)}</pre><p class="border-t border-border px-4 py-2 text-[10px] text-muted-foreground">${escapeHtml(logNote)}</p>`
      : `<p class="px-4 py-3 text-xs text-muted-foreground">${run.status === "queued" ? "Queued: waiting for a free runner or for the workflows this one needs." : run.status === "awaiting-approval" ? "Nothing has run yet." : "This run produced no log output."}</p>`;
    return `
      <div data-repo-actions-run-detail="${run.id}" class="border-t border-border bg-background">
        <div class="flex flex-wrap items-center justify-end gap-2 border-b border-border px-4 py-2">${renderRepoActionsRunControls(view, run)}</div>
        ${approval}${steps}${log}
      </div>`;
  }

  function renderRepoActionsRuns(repo, view, workflow) {
    const runs = view.runs.filter((run) => repoActionsRunMatchesWorkflow(run, workflow));
    if (!runs.length) {
      return `<p class="px-4 py-6 text-center text-xs text-muted-foreground">${view.access === "public" ? "No run yet." : view.source === "runner" && view.controlAvailable ? "No run yet. Push, wait for a schedule, or pick a workflow and run it." : view.source === "runner" ? "No runner run yet. The next push to the default branch while the desktop is away runs here." : workflow ? "This workflow has no recent run on the executor." : "No recent runs on the executor."}</p>`;
    }
    return runs.map((run) => {
      const meta = pullCheckStateMeta(run.status);
      const open = view.selectedRunId === run.id;
      const when = run.finishedAt || run.startedAt || run.createdAt;
      const duration = repoActionsDuration(run);
      // The document click handler opens the commit page for this attribute
      // from any tab.
      const commitLink = run.commit
        ? `<button type="button" data-dashboard-commit-hash="${escapeHtml(run.commit)}" class="font-mono hover:text-foreground hover:underline">${escapeHtml(run.commit.slice(0, 10))}</button>`
        : "";
      const pullLink = run.pullNumber
        ? `<a href="${escapeHtml(`${repoLinkUrl(repo)}/pulls/${run.pullNumber}`)}" data-repo-actions-link class="hover:text-foreground hover:underline">#${run.pullNumber}</a>`
        : "";
      const facts = [
        `run ${run.id}`, commitLink, escapeHtml(repoActionsRefLabel(run.ref)), pullLink,
        run.actor ? `<span data-repo-actions-run-actor>${userNameHtml(run.actor)}</span>` : "",
        when ? escapeHtml(relativeTimeLabel(when)) : "", duration ? escapeHtml(duration) : "",
      ].filter(Boolean).join(" · ");
      return `
        <div data-repo-actions-run="${run.id}" class="border-b border-border last:border-b-0">
          <button type="button" data-repo-actions-run-toggle="${run.id}" aria-expanded="${open ? "true" : "false"}" class="flex w-full min-w-0 items-center gap-3 px-4 pb-1 pt-3 text-left hover:bg-secondary/40">
            <i data-lucide="${meta.icon}" class="h-4 w-4 shrink-0 ${meta.tone}${run.status === "running" ? " animate-spin" : ""}"></i>
            <span class="flex min-w-0 flex-1 flex-wrap items-center gap-2 text-sm font-medium text-foreground"><span class="min-w-0 truncate">${escapeHtml(run.workflow)}</span><span class="rounded-full border border-border px-2 py-0.5 text-[10px] font-semibold ${meta.tone}">${escapeHtml(meta.label)}</span>${run.event ? `<span data-repo-actions-run-event="${escapeHtml(run.event)}" class="fm-label font-mono text-[10px] text-muted-foreground">${escapeHtml(run.event)}</span>` : ""}</span>
            <i data-lucide="${open ? "chevron-up" : "chevron-down"}" class="h-4 w-4 shrink-0 text-muted-foreground"></i>
          </button>
          <div class="pb-3 pl-11 pr-4 text-[11px] text-muted-foreground">${facts}</div>
          ${open ? renderRepoActionsRunDetail(repo, view, run) : ""}
        </div>`;
    }).join("");
  }

  function renderRepoActionsEmptyState(view) {
    const message = {
      "signed-out": ["lock", "Sign in to see runs", "Workflow runs, logs and controls are shown to the repository owner and organization writers."],
      forbidden: ["lock", "Runs are private to this repository's writers", "Workflow runs can print build output, so only the owner and organization writers can read them."],
      unavailable: ["server-off", "No runs reported for this repository yet", `A push to the default branch runs the workflows below on the owner's desktop while it is connected, and on a ${platformName()} runner while it is away. Desktop runs show in the desktop app and on paired phones; runner and mirror runs appear here. To run them here while the desktop is away, link a runner that allows this owner.`],
      error: ["triangle-alert", "Actions are unreachable right now", view.error],
      loading: ["loader-circle", "Asking the mirror executor ...", ""],
    }[view.phase];
    if (!message) return "";
    return `<div class="fm-blankslate py-10"><i data-lucide="${message[0]}"${view.phase === "loading" ? ' class="animate-spin"' : ""}></i><p class="fm-blankslate-title">${escapeHtml(message[1])}</p>${message[2] ? `<p class="fm-blankslate-desc">${escapeHtml(message[2])}</p>` : ""}${view.phase === "loading" || view.phase === "signed-out" || view.phase === "forbidden" ? "" : `<button type="button" data-repo-actions-refresh class="fm-btn fm-btn-sm mt-3"><i data-lucide="refresh-cw"></i>Try again</button>`}</div>`;
  }

  function renderRepoActions(repo) {
    const target = $("[data-repo-actions]");
    if (!target || !repoActionsIsCurrent(repo)) return;
    const view = repoActionsView();
    const workflows = repoActionsWorkflows(view);
    if (view.selectedWorkflow && !workflows.some((entry) => entry.path === view.selectedWorkflow)) {
      view.selectedWorkflow = "";
    }
    const workflow = workflows.find((entry) => entry.path === view.selectedWorkflow) || null;
    // Re-rendering replaces the log element; keep a reader's scroll position,
    // and keep following the tail for someone who was already at the bottom.
    const previousLog = target.querySelector("[data-repo-actions-log]");
    const logScroll = previousLog
      ? { top: previousLog.scrollTop, pinned: previousLog.scrollTop + previousLog.clientHeight >= previousLog.scrollHeight - 8 }
      : null;
    // A poll re-renders under the reader; do not reset the branch they picked.
    const chosenRef = target.querySelector("[data-repo-actions-dispatch-ref]")?.value || "";
    const notice = view.notice
      ? `<div role="status" class="fm-flash fm-flash-full ${view.notice.tone === "good" ? "fm-flash-success" : view.notice.tone === "bad" ? "fm-flash-danger" : "fm-flash-neutral"} items-center border-t-0 px-4 py-2 text-xs"><i data-lucide="${view.notice.tone === "good" ? "circle-check" : view.notice.tone === "bad" ? "circle-x" : "info"}" class="!mt-0"></i><span class="fm-flash-body">${escapeHtml(view.notice.text)}</span><button type="button" data-repo-actions-notice-dismiss aria-label="Dismiss" class="fm-btn fm-btn-sm fm-btn-icon fm-btn-invisible"><i data-lucide="x"></i></button></div>`
      : "";
    const pending = view.pending.length
      ? `<div role="status" class="fm-flash fm-flash-neutral fm-flash-full items-center border-t-0 px-4 py-2 text-xs"><i data-lucide="loader-circle" class="!mt-0 animate-spin"></i><span class="fm-flash-body">${escapeHtml(view.pending.map((entry) => entry.label).join(", "))}: sent to ${escapeHtml(view.node)}, waiting for the executor ...</span></div>`
      : "";
    const main = view.phase === "ready"
      ? `${renderRepoActionsExecutor(repo, view)}${notice}${pending}${renderRepoActionsWorkflowBar(repo, view, workflow)}<div data-repo-actions-runs>${renderRepoActionsRuns(repo, view, workflow)}</div>`
      : `${notice}${renderRepoActionsEmptyState(view)}`;
    target.innerHTML = `
      <div class="grid min-w-0 md:grid-cols-[16rem_minmax(0,1fr)]">
        <nav aria-label="Workflows" data-repo-actions-workflows class="grid content-start gap-0.5 border-b border-border p-2 md:border-b-0 md:border-r">${renderRepoActionsWorkflowList(view)}</nav>
        <div class="min-w-0">${main}</div>
      </div>`;
    const nextRef = target.querySelector("[data-repo-actions-dispatch-ref]");
    if (nextRef && chosenRef && Array.from(nextRef.options).some((option) => option.value === chosenRef)) {
      nextRef.value = chosenRef;
    }
    const nextLog = target.querySelector("[data-repo-actions-log]");
    if (nextLog) {
      nextLog.scrollTop = !logScroll || logScroll.pinned ? nextLog.scrollHeight : logScroll.top;
    }
    window.lucide?.createIcons();
  }

  // Click/submit routing for everything inside [data-repo-actions]. Returns
  // true when the event belonged to this tab.
  function handleRepoActionsClick(event) {
    const root = event.target.closest?.("[data-repo-actions]");
    if (!root) return false;
    const repo = state.selectedRepo;
    const view = repoActionsView();
    if (!repo) return false;
    if (event.target.closest("[data-repo-actions-link], [data-dashboard-commit-hash]")) return false;
    if (event.target.closest("[data-repo-actions-refresh]")) {
      view.notice = null;
      void loadRepoActions(repo);
      return true;
    }
    if (event.target.closest("[data-repo-actions-notice-dismiss]")) {
      view.notice = null;
      renderRepoActions(repo);
      return true;
    }
    const workflowButton = event.target.closest("[data-repo-actions-workflow]");
    if (workflowButton) {
      view.selectedWorkflow = workflowButton.dataset.repoActionsWorkflow || "";
      renderRepoActions(repo);
      return true;
    }
    const runToggle = event.target.closest("[data-repo-actions-run-toggle]");
    if (runToggle) {
      const id = Number(runToggle.dataset.repoActionsRunToggle) || 0;
      view.selectedRunId = view.selectedRunId === id ? 0 : id;
      renderRepoActions(repo);
      if (view.selectedRunId && view.source === "runner" && view.access !== "public") void loadRepoActionsRunLog(repo, view.selectedRunId);
      return true;
    }
    const runControl = event.target.closest("[data-repo-actions-run-control]");
    if (runControl) {
      const action = runControl.dataset.repoActionsRunControl;
      const runId = Number(runControl.dataset.repoActionsRunId) || 0;
      const run = view.runs.find((entry) => entry.id === runId);
      if (!run || !["approve", "reject", "cancel", "rerun"].includes(action)) return true;
      const label = {
        approve: `Approve run ${runId}`, reject: `Reject run ${runId}`,
        cancel: `Stop run ${runId}`, rerun: `Re-run of ${run.workflow}`,
      }[action];
      // Approval executes code nobody has run here before; make that a
      // deliberate second step rather than a stray click.
      if (action === "approve" && !window.confirm(
        `Approve run ${runId} of "${run.workflow}" at ${run.commit.slice(0, 10)}? It will execute on ${view.node}.`)) {
        return true;
      }
      void sendRepoActionsControl(repo, action, { runId }, label);
      return true;
    }
    const workflowEnabled = event.target.closest("[data-repo-actions-workflow-enabled]");
    if (workflowEnabled) {
      const enabled = workflowEnabled.dataset.repoActionsWorkflowEnabled === "true";
      const workflow = workflowEnabled.dataset.repoActionsWorkflowPath || "";
      if (REPO_ACTIONS_WORKFLOW_PATH.test(workflow)) {
        void sendRepoActionsControl(repo, "set-workflow-enabled", { workflow, enabled },
          `${enabled ? "Enable" : "Disable"} ${workflow.split("/").pop()}`);
      }
      return true;
    }
    const repositoryEnabled = event.target.closest("[data-repo-actions-repository-enabled]");
    if (repositoryEnabled) {
      const enabled = repositoryEnabled.dataset.repoActionsRepositoryEnabled === "true";
      void sendRepoActionsControl(repo, "set-repository-enabled", { enabled },
        enabled ? `Run Actions on ${view.node}` : `Pause Actions on ${view.node}`);
      return true;
    }
    return false;
  }

  function handleRepoActionsSubmit(event) {
    const form = event.target.closest?.("[data-repo-actions-dispatch]");
    if (!form) return false;
    event.preventDefault();
    const repo = state.selectedRepo;
    const workflow = form.dataset.repoActionsDispatch || "";
    const branch = String(form.querySelector("[data-repo-actions-dispatch-ref]")?.value || "");
    if (!repo || !REPO_ACTIONS_WORKFLOW_PATH.test(workflow) || !branch) return true;
    const view = repoActionsView();
    const label = `Run ${workflow.split("/").pop()} on ${branch}`;
    if (view.source === "runner") {
      const inputs = {};
      const missing = [];
      form.querySelectorAll("[data-repo-actions-input]").forEach((field) => {
        const name = field.dataset.repoActionsInput || "";
        if (!REPO_ACTIONS_INPUT_NAME.test(name)) return;
        if (field.dataset.repoActionsInputType === "boolean") {
          inputs[name] = Boolean(field.checked);
          return;
        }
        const value = String(field.value || "");
        if (field.hasAttribute("data-repo-actions-input-required") && !value.trim()) missing.push(name);
        inputs[name] = value;
      });
      if (missing.length) {
        view.notice = { tone: "bad", text: `${label}: fill in ${missing.join(", ")}.` };
        renderRepoActions(repo);
        return true;
      }
      void sendRepoActionsControl(repo, "dispatch", { workflow, ref: branch, inputs }, label);
      return true;
    }
    void sendRepoActionsControl(repo, "dispatch",
      { workflow, ref: `refs/heads/${branch}` },
      label);
    return true;
  }

  function actionsSecretsState(container) {
    if (!container.__actionsSecrets) container.__actionsSecrets = { secrets: [], loaded: false, error: "" };
    return container.__actionsSecrets;
  }

  function actionsSecretsBase(container) {
    const base = String(container?.dataset?.actionsSecretsBase || "");
    return /^\/api\/(?:repo\/[^/]+\/[^/]+|org\/[^/]+)\/actions\/secrets$/.test(base) ? base : "";
  }

  function renderActionsSecrets(container) {
    const data = actionsSecretsState(container);
    const scope = container.dataset.actionsSecretsScope === "org" ? "organization" : "repository";
    const rows = data.secrets.map((secret) => `
      <div data-actions-secret-row="${escapeHtml(secret.name)}" class="flex min-w-0 flex-wrap items-center justify-between gap-2 px-3 py-2">
        <span class="min-w-0 truncate font-mono text-xs text-foreground">${escapeHtml(secret.name)}</span>
        <span class="flex shrink-0 items-center gap-2">
          <span class="text-[10px] text-muted-foreground">${secret.updatedAt ? `updated ${escapeHtml(relativeTimeLabel(secret.updatedAt))}` : ""}</span>
          <button type="button" data-actions-secret-edit="${escapeHtml(secret.name)}" class="fm-btn fm-btn-sm">Update</button>
          <button type="button" data-actions-secret-delete="${escapeHtml(secret.name)}" class="fm-btn fm-btn-sm fm-btn-danger">Delete</button>
        </span>
      </div>`).join("");
    const list = !data.loaded
      ? `<p class="px-3 py-2 text-[11px] text-muted-foreground">${data.error ? escapeHtml(data.error) : "Loading secrets ..."}</p>`
      : rows
        ? `<div data-actions-secrets-list class="divide-y divide-border border-y border-border">${rows}</div>`
        : `<p data-actions-secrets-list class="text-[11px] text-muted-foreground">No secrets yet.</p>`;
    container.innerHTML = `
      <div class="grid gap-3">
        <p class="text-[11px] leading-5 text-muted-foreground">Workflows read these as <span class="font-mono">\${{ secrets.NAME }}</span> on this instance's runner. Values are encrypted, never shown again, and masked in logs. ${scope === "organization" ? "A repository secret with the same name wins over an organization secret." : "Organization secrets with the same name are overridden by these."}</p>
        ${list}
        <form data-actions-secret-form class="grid gap-2">
          <label class="grid gap-1 text-[11px] font-medium text-foreground">Name
            <input data-actions-secret-name type="text" autocomplete="off" spellcheck="false" maxlength="64" pattern="[A-Z_][A-Z0-9_]*" required placeholder="CLOUDFLARE_API_TOKEN" class="h-control w-full rounded-md border border-border bg-background px-2 font-mono text-control text-foreground" />
          </label>
          <label class="grid gap-1 text-[11px] font-medium text-foreground">Value
            <textarea data-actions-secret-value rows="3" autocomplete="off" spellcheck="false" required class="w-full rounded-md border border-border bg-background px-2 py-1.5 font-mono text-control text-foreground"></textarea>
          </label>
          <div class="flex flex-wrap items-center justify-between gap-2">
            <span data-actions-secrets-status class="text-[11px] ${data.tone === "bad" ? "text-destructive" : "text-muted-foreground"}">${escapeHtml(data.status || "")}</span>
            <button type="submit" class="fm-btn fm-btn-sm">Save secret</button>
          </div>
        </form>
      </div>`;
  }

  async function loadActionsSecrets(container) {
    const base = actionsSecretsBase(container);
    if (!base) return;
    const data = actionsSecretsState(container);
    try {
      const payload = await orgAgentRequest("GET", base);
      data.secrets = (Array.isArray(payload?.secrets) ? payload.secrets : [])
        .map((entry) => ({
          name: String(entry?.name || ""),
          updatedAt: Math.max(0, Number(entry?.updatedAt ?? entry?.updated_at) || 0),
        }))
        .filter((entry) => ACTIONS_SECRET_NAME.test(entry.name))
        .sort((left, right) => left.name.localeCompare(right.name));
      data.loaded = true;
      data.error = "";
    } catch (error) {
      data.loaded = false;
      data.error = Number(error?.status) === 403
        ? "Only the repository owner or an organization admin can manage secrets."
        : "The secrets could not be loaded.";
    }
    if (container.isConnected) renderActionsSecrets(container);
  }

  function mountActionsSecrets(container) {
    if (!container || !actionsSecretsBase(container)) return;
    renderActionsSecrets(container);
    if (!container.__actionsSecretsWired) {
      container.__actionsSecretsWired = true;
      container.addEventListener("submit", (event) => {
        if (!event.target.closest("[data-actions-secret-form]")) return;
        event.preventDefault();
        void saveActionsSecret(container, event.target.closest("[data-actions-secret-form]"));
      });
      container.addEventListener("click", (event) => {
        const edit = event.target.closest("[data-actions-secret-edit]");
        if (edit) {
          const field = container.querySelector("[data-actions-secret-name]");
          if (field) field.value = edit.dataset.actionsSecretEdit || "";
          container.querySelector("[data-actions-secret-value]")?.focus();
          return;
        }
        const remove = event.target.closest("[data-actions-secret-delete]");
        if (remove) void deleteActionsSecret(container, remove.dataset.actionsSecretDelete || "");
      });
    }
    void loadActionsSecrets(container);
  }

  function setActionsSecretsStatus(container, text, tone = "") {
    const data = actionsSecretsState(container);
    data.status = text;
    data.tone = tone;
    const status = container.querySelector("[data-actions-secrets-status]");
    if (status) {
      status.textContent = text;
      status.className = `text-[11px] ${tone === "bad" ? "text-destructive" : tone === "good" ? "text-success" : "text-muted-foreground"}`;
    }
  }

  async function saveActionsSecret(container, form) {
    const base = actionsSecretsBase(container);
    const nameField = form.querySelector("[data-actions-secret-name]");
    const valueField = form.querySelector("[data-actions-secret-value]");
    const name = String(nameField?.value || "").trim();
    const value = String(valueField?.value || "");
    if (!base || !ACTIONS_SECRET_NAME.test(name)) {
      setActionsSecretsStatus(container, "Names use capital letters, digits and _, and do not start with a digit.", "bad");
      return;
    }
    if (!value || new TextEncoder().encode(value).length > ACTIONS_SECRET_MAX_BYTES) {
      setActionsSecretsStatus(container, "The value must be between 1 byte and 48 KiB.", "bad");
      return;
    }
    const submit = form.querySelector('button[type="submit"]');
    if (submit) submit.disabled = true;
    setActionsSecretsStatus(container, "Saving ...");
    try {
      await orgAgentRequest("PUT", `${base}/${encodeURIComponent(name)}`, { value });
      if (valueField) valueField.value = "";
      if (nameField) nameField.value = "";
      setActionsSecretsStatus(container, `Saved ${name}.`, "good");
      await loadActionsSecrets(container);
    } catch (error) {
      if (valueField) valueField.value = "";
      setActionsSecretsStatus(container, error?.message || `${name} could not be saved.`, "bad");
    } finally {
      if (submit) submit.disabled = false;
    }
  }

  async function deleteActionsSecret(container, name) {
    const base = actionsSecretsBase(container);
    if (!base || !ACTIONS_SECRET_NAME.test(name)) return;
    if (!window.confirm(`Delete the secret ${name}? Workflows that read it get an empty value.`)) return;
    setActionsSecretsStatus(container, "Deleting ...");
    try {
      await orgAgentRequest("DELETE", `${base}/${encodeURIComponent(name)}`);
      setActionsSecretsStatus(container, `Deleted ${name}.`, "good");
      await loadActionsSecrets(container);
    } catch (error) {
      setActionsSecretsStatus(container, error?.message || `${name} could not be deleted.`, "bad");
    }
  }


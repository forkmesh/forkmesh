  // ---------------------------------------------------------------------------
  // /dashboard/runners: list the agent runners this account may manage and
  // sign their provider CLIs in from the browser (app/src/runner_logins.py).
  // The runner runs the provider's own login on a pty and relays the screen in
  // numbered chunks; this page shows it, turns https links into anchors, and
  // posts the pasted code back. Polling runs only while a sign-in panel is
  // open, and stops when the session ends or the panel closes.
  const RUNNER_PROVIDERS = Object.freeze([
    { id: "claude-code", label: "Claude Code", button: "Connect Claude" },
    { id: "codex", label: "Codex", button: "Connect Codex" },
  ]);
  const RUNNER_LOGIN_POLL_MS = 1000;
  const RUNNER_LOGIN_MAX_TEXT = 200000;
  const RUNNER_LOGIN_ACTIVE = new Set(["pending", "running"]);
  const RUNNER_LOGIN_STATES = Object.freeze({
    pending: "Waiting for the runner to pick this up. It checks in about every 20 seconds.",
    running: "The runner is running the sign-in. Open the link below, approve it, and paste the code.",
    succeeded: "Signed in. The credential stays on the runner, which adds it to its accounts and makes it the active one.",
    failed: "The sign-in did not finish. Read the terminal above, then try again.",
    expired: "The sign-in ran out of time (10 minutes). Start it again when you are ready.",
    cancelled: "The sign-in was cancelled.",
  });
  const RUNNER_ACCOUNT_POLL_MS = 2000;
  const RUNNER_USAGE_ERRORS = Object.freeze({
    token_expired: "Usage refreshes after this account's next run (its access token has expired).",
    unauthorized: "The provider refused this login. Sign this account in again.",
    no_oauth_login: "Usage limits are shown for subscription sign-ins only.",
    missing_login: "The saved login is missing. Sign this account in again.",
    unreachable: "The provider's usage service could not be reached.",
  });
  const runnersView = {
    runners: [],
    login: null, // { runner, provider, id, seq, state, text, timer, decoder }
    refreshTimer: 0,
    focus: null,
  };

  async function runnersRequest(method, path, body) {
    const headers = { accept: "application/json" };
    const token = state.session?.sessionToken || "";
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    const init = { method, headers, cache: "no-store", credentials: "same-origin" };
    if (body !== undefined) {
      headers["content-type"] = "application/json";
      init.body = JSON.stringify(body);
    }
    const response = await fetch(path, init);
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(payload.message || payload.error || `HTTP ${response.status}`);
      error.status = response.status;
      error.code = String(payload.error || "");
      error.payload = payload;
      throw error;
    }
    return payload;
  }

  function runnersEl(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = String(text);
    return node;
  }

  function setRunnersNotice(message, isError) {
    const notice = $("[data-runners-notice]");
    if (!notice) return;
    notice.hidden = !message;
    notice.textContent = message || "";
    notice.classList.toggle("text-red-500", Boolean(isError));
    notice.classList.toggle("text-muted-foreground", !isError);
  }

  function runnerSeenLabel(ms) {
    if (!ms) return "never checked in";
    const seconds = Math.max(0, Math.round((Date.now() - ms) / 1000));
    if (seconds < 90) return `checked in ${seconds}s ago`;
    const minutes = Math.round(seconds / 60);
    if (minutes < 90) return `checked in ${minutes} min ago`;
    return `checked in ${Math.round(minutes / 60)} h ago`;
  }

  function runnerAuthState(entry) {
    if (!entry) return "";
    if (entry.loginState === "expired") return "expired";
    return String(entry.auth?.state || "");
  }

  function runnerExpiryLabel(ms) {
    if (!ms) return "";
    try {
      return new Date(ms).toLocaleString(undefined, { dateStyle: "medium", timeStyle: "short" });
    } catch {
      return new Date(ms).toISOString();
    }
  }

  function runnerProviderStatus(runner, provider) {
    const entry = runner.availability?.[provider.id];
    if (!entry) return { text: "Not reported yet", ok: false };
    if (!entry.binaryFound) return { text: `${provider.label} is not installed`, ok: false, missing: true };
    const auth = runnerAuthState(entry);
    if (auth === "expired") {
      return { text: "Login expired — reconnect", ok: false, expired: true };
    }
    if (entry.loginState === "available") {
      if (auth === "expiring") {
        const when = runnerExpiryLabel(entry.auth?.expiresAt);
        return { text: `Signed in — login expires ${when || "within a day"}`, ok: true, expiring: true };
      }
      return { text: `Signed in${entry.source ? ` (${entry.source})` : ""}`, ok: true };
    }
    return { text: "Not signed in", ok: false };
  }

  function renderRunnersAuthBanner() {
    const banner = $("[data-runners-auth-banner]");
    if (!banner) return;
    banner.replaceChildren();
    const problems = [];
    for (const runner of runnersView.runners) {
      if (runner.status !== "active") continue;
      for (const provider of RUNNER_PROVIDERS) {
        const status = runnerProviderStatus(runner, provider);
        if (status.expired || status.expiring) problems.push({ runner, provider, status });
      }
    }
    banner.hidden = problems.length === 0;
    const expired = problems.some((problem) => problem.status.expired);
    banner.classList.toggle("bg-red-500/10", expired);
    banner.classList.toggle("bg-amber-500/10", !expired && problems.length > 0);
    for (const { runner, provider, status } of problems) {
      const row = runnersEl("div", "flex flex-wrap items-center gap-2");
      row.dataset.runnerAuthProblem = `${runner.name}:${provider.id}`;
      const short = provider.id === "codex" ? "Codex" : "Claude";
      const text = status.expired
        ? `${runner.name}: ${short} login expired — reconnect. Jobs for ${provider.label} are not running on this runner.`
        : `${runner.name}: ${short} login expires soon. ${status.text}.`;
      row.append(runnersEl("span",
        "font-semibold " + (status.expired ? "text-red-500" : "text-amber-600"), text));
      const active = (runner.logins || []).find(
        (login) => login.provider === provider.id && RUNNER_LOGIN_ACTIVE.has(login.state));
      const button = runnersEl("button",
        "ml-auto inline-flex items-center gap-1.5 rounded-md bg-primary px-3 py-1.5 text-xs font-semibold text-primary-foreground hover:opacity-90 transition-opacity",
        active ? "Open sign-in" : provider.button);
      button.type = "button";
      button.dataset.runnerAuthConnect = provider.id;
      button.addEventListener("click", () => {
        if (active) {
          attachRunnerLogin(runner.name, provider.id, active.id);
        } else {
          void startRunnerLogin(runner.name, provider.id);
        }
      });
      row.append(button);
      banner.append(row);
    }
  }

  function runnerDurationLabel(ms) {
    const minutes = Math.max(1, Math.round(ms / 60000));
    if (minutes < 60) return `${minutes} min`;
    const hours = Math.floor(minutes / 60);
    if (hours < 48) return `${hours} h ${minutes % 60} min`;
    return `${Math.round(hours / 24)} days`;
  }

  function runnerUsageRow(window) {
    const used = Math.max(0, Math.min(100, Number(window.usedPercent) || 0));
    const row = runnersEl("div", "grid gap-1");
    const line = runnersEl("div", "flex flex-wrap items-center gap-2 text-xs");
    line.append(runnersEl("span", "text-foreground", window.label || window.id));
    line.append(runnersEl("span", used >= 90 ? "text-red-500 font-semibold" : "text-muted-foreground",
      `${Math.round(used)}% used`));
    if (window.resetsAt) {
      const left = window.resetsAt - Date.now();
      line.append(runnersEl("span", "text-muted-foreground",
        left > 0 ? `resets in ${runnerDurationLabel(left)}` : "reset since this reading"));
    }
    row.append(line);
    const bar = runnersEl("div", "h-1.5 w-full max-w-xs overflow-hidden rounded-full bg-secondary");
    bar.setAttribute("role", "meter");
    bar.setAttribute("aria-valuemin", "0");
    bar.setAttribute("aria-valuemax", "100");
    bar.setAttribute("aria-valuenow", String(Math.round(used)));
    bar.setAttribute("aria-label", `${window.label || window.id} usage`);
    const fill = runnersEl("div", "h-full rounded-full " + (used >= 90 ? "bg-red-500" : "bg-primary"));
    fill.style.width = `${used}%`;
    bar.append(fill);
    row.append(bar);
    return row;
  }

  function runnerAccountUsage(account) {
    const usage = account.usage;
    const box = runnersEl("div", "grid gap-1.5");
    if (!usage) {
      box.append(runnersEl("p", "text-xs text-muted-foreground", "Usage limits have not been read yet."));
      return box;
    }
    for (const window of usage.windows || []) box.append(runnerUsageRow(window));
    const notes = [];
    if (usage.error) notes.push(RUNNER_USAGE_ERRORS[usage.error] || `Usage could not be read (${usage.error}).`);
    if (usage.fetchedAt) {
      notes.push(`Read ${runnerDurationLabel(Math.max(0, Date.now() - usage.fetchedAt))} ago.`);
    }
    if (!(usage.windows || []).length && !usage.error) notes.push("The provider reported no usage limits.");
    if (notes.length) box.append(runnersEl("p", "text-xs text-muted-foreground", notes.join(" ")));
    return box;
  }

  function renderRunnerAccounts(runner, provider) {
    const accounts = runner.availability?.[provider.id]?.accounts || [];
    const wrap = runnersEl("div", "grid gap-2 pl-3 border-l border-border");
    const pending = (runner.accountRequests || []).filter(
      (request) => request.provider === provider.id && request.state === "pending");
    for (const account of accounts) {
      const item = runnersEl("div", "grid gap-1.5");
      item.dataset.runnerAccount = account.id;
      const head = runnersEl("div", "flex flex-wrap items-center gap-2");
      head.append(runnersEl("span", "text-sm text-foreground", account.label || account.email || "Account"));
      if (account.plan) {
        head.append(runnersEl("span",
          "rounded-full border border-border px-2 py-0.5 text-[10px] font-semibold uppercase tracking-wide text-muted-foreground",
          account.plan));
      }
      const waiting = pending.find((request) => request.account === account.id);
      if (account.active) {
        head.append(runnersEl("span", "text-xs font-semibold text-emerald-500", "Active"));
      }
      if (waiting) {
        head.append(runnersEl("span", "text-xs text-muted-foreground",
          waiting.action === "remove" ? "Removing..." : "Switching..."));
      }
      const actions = runnersEl("div", "ml-auto flex items-center gap-2");
      const buttonClass = "inline-flex items-center rounded-md border border-border px-2.5 py-1 text-xs font-semibold text-muted-foreground hover:bg-secondary hover:text-foreground transition-colors";
      if (!account.active) {
        const use = runnersEl("button", buttonClass, "Use this account");
        use.type = "button";
        use.dataset.runnerAccountActivate = account.id;
        use.disabled = runner.status !== "active" || Boolean(waiting);
        use.addEventListener("click", () => {
          void requestRunnerAccount(runner.name, provider.id, account, "activate");
        });
        actions.append(use);
      }
      const remove = runnersEl("button", buttonClass, "Remove");
      remove.type = "button";
      remove.dataset.runnerAccountRemove = account.id;
      remove.disabled = runner.status !== "active" || Boolean(waiting);
      remove.addEventListener("click", () => {
        void requestRunnerAccount(runner.name, provider.id, account, "remove");
      });
      actions.append(remove);
      head.append(actions);
      item.append(head);
      item.append(runnerAccountUsage(account));
      wrap.append(item);
    }
    for (const request of runner.accountRequests || []) {
      if (request.provider === provider.id && request.state === "failed" && request.message) {
        wrap.append(runnersEl("p", "text-xs text-red-500", `Could not ${request.action === "remove" ? "remove" : "switch"} the account: ${request.message}`));
      }
    }
    return wrap;
  }

  async function requestRunnerAccount(runner, provider, account, action) {
    const name = account.label || account.email || "this account";
    if (action === "remove" && !window.confirm(
      `Remove ${name} from ${runner}? The runner deletes its saved login. Sign it in again to use it later.`)) return;
    setRunnersNotice("");
    try {
      await runnersRequest("POST", `/api/runners/${encodeURIComponent(runner)}/accounts`,
        { provider, account: account.id, action });
    } catch (error) {
      setRunnersNotice(`Could not ${action === "remove" ? "remove" : "switch"} the account: ${error.message}`, true);
    }
    void loadRunners();
  }

  function scheduleRunnersRefresh() {
    if (runnersView.refreshTimer) window.clearTimeout(runnersView.refreshTimer);
    runnersView.refreshTimer = 0;
    const waiting = runnersView.runners.some((runner) =>
      (runner.accountRequests || []).some((request) => request.state === "pending"));
    if (waiting && $("[data-runners-list]")) {
      runnersView.refreshTimer = window.setTimeout(() => { void loadRunners(); }, RUNNER_ACCOUNT_POLL_MS);
    }
  }

  function renderRunners() {
    const list = $("[data-runners-list]");
    if (!list) return;
    const setupName = $("[data-runner-setup-name]");
    if (setupName?.dataset.auto === "1") setupName.value = runnerSetupDefaultName();
    renderRunnersAuthBanner();
    list.replaceChildren();
    if (!runnersView.runners.length) {
      list.append(runnersEl("p", "text-sm text-muted-foreground",
        "No runners you can manage. Administrators see every runner; others see the personal runners they registered."));
      return;
    }
    for (const runner of runnersView.runners) {
      const card = runnersEl("div", "rounded-md border border-border bg-background p-3 grid gap-2");
      card.dataset.runner = runner.name;
      const head = runnersEl("div", "flex flex-wrap items-center gap-2");
      head.append(runnersEl("span", "font-mono text-sm font-semibold text-foreground", runner.name));
      head.append(runnersEl("span",
        "rounded-full border border-border bg-secondary px-2 py-0.5 text-[10px] font-semibold uppercase tracking-wide text-muted-foreground",
        runner.scope));
      if (runner.status !== "active") {
        head.append(runnersEl("span", "text-xs text-red-500", runner.status));
      }
      head.append(runnersEl("span", "text-xs text-muted-foreground", runnerSeenLabel(runner.lastSeenAt)));
      card.append(head);
      for (const provider of RUNNER_PROVIDERS) {
        const row = runnersEl("div", "flex flex-wrap items-center gap-2");
        const status = runnerProviderStatus(runner, provider);
        row.append(runnersEl("span", "text-sm text-foreground", provider.label));
        row.append(runnersEl("span",
          "text-xs " + (status.expired ? "text-red-500 font-semibold"
            : status.expiring ? "text-amber-600 font-semibold"
              : status.ok ? "text-emerald-500" : "text-muted-foreground"), status.text));
        const active = (runner.logins || []).find(
          (login) => login.provider === provider.id && RUNNER_LOGIN_ACTIVE.has(login.state));
        const hasAccounts = (runner.availability?.[provider.id]?.accounts || []).length > 0;
        const button = runnersEl("button",
          "ml-auto inline-flex items-center gap-1.5 rounded-md border border-border px-3 py-1.5 text-xs font-semibold text-muted-foreground hover:bg-secondary hover:text-foreground transition-colors",
          active ? "Open sign-in" : (status.expired ? provider.button
            : (hasAccounts ? "Add account" : (status.ok ? "Sign in again" : provider.button))));
        button.title = hasAccounts && !active
          ? "Sign in another account, or the same one again to renew its login. The new sign-in becomes the active account."
          : "";
        button.type = "button";
        button.dataset.runnerConnect = provider.id;
        button.disabled = runner.status !== "active" || Boolean(status.missing && !active);
        button.addEventListener("click", () => {
          if (active) {
            attachRunnerLogin(runner.name, provider.id, active.id);
          } else {
            void startRunnerLogin(runner.name, provider.id);
          }
        });
        row.append(button);
        card.append(row);
        if (hasAccounts) card.append(renderRunnerAccounts(runner, provider));
      }
      list.append(card);
    }
  }

  async function loadRunners() {
    try {
      const payload = await runnersRequest("GET", "/api/runners");
      runnersView.runners = Array.isArray(payload.runners) ? payload.runners : [];
      setRunnersNotice("");
    } catch (error) {
      runnersView.runners = [];
      setRunnersNotice(error.status === 401
        ? "Sign in to manage runners."
        : `Could not load runners: ${error.message}`, true);
      renderRunners();
      return;
    }
    renderRunners();
    focusRequestedRunner();
    scheduleRunnersRefresh();
  }

  function requestedRunnerFocus() {
    const params = new URLSearchParams(window.location.search);
    const provider = String(params.get("provider") || "");
    if (!RUNNER_PROVIDERS.some((entry) => entry.id === provider)) return null;
    return {
      runner: String(params.get("runner") || "").trim().toLowerCase(),
      provider,
      connect: params.get("connect") === "1",
    };
  }

  function dropConnectParam() {
    const url = new URL(window.location.href);
    if (!url.searchParams.has("connect") && !url.searchParams.has("setup")) return;
    url.searchParams.delete("connect");
    url.searchParams.delete("setup");
    window.history.replaceState(window.history.state, "", url.pathname + url.search + url.hash);
  }

  function runnerForTarget(runners, target, providerId) {
    const provider = RUNNER_PROVIDERS.find((entry) => entry.id === providerId);
    const active = runners.filter((runner) => runner.status === "active");
    const named = target && active.find((runner) => String(runner.name).toLowerCase() === target);
    if (named) return named.name;
    if (active.length === 1) return active[0].name;
    const unsigned = active.filter((runner) => {
      const status = runnerProviderStatus(runner, provider);
      return !status.ok && !status.missing;
    });
    return unsigned.length === 1 ? unsigned[0].name : "";
  }

  function focusRequestedRunner() {
    const focus = runnersView.focus;
    if (!focus) return;
    runnersView.focus = null;
    const provider = RUNNER_PROVIDERS.find((entry) => entry.id === focus.provider);
    const name = runnerForTarget(runnersView.runners, focus.runner, focus.provider);
    const card = name
      ? $$("[data-runner]").find((node) => node.dataset.runner === name)
      : null;
    if (!card) {
      setRunnersNotice(focus.runner
        ? `${focus.runner} is not a runner you can manage. Ask an administrator or the runner's registrant to sign ${provider.label} in again.`
        : `Pick the runner that ran the job and sign ${provider.label} in again.`, Boolean(focus.runner));
      return;
    }
    card.classList.add("ring-2", "ring-primary");
    card.scrollIntoView({ block: "center" });
    const button = card.querySelector(`[data-runner-connect="${provider.id}"]`);
    if (button && !button.disabled) button.focus();
    setRunnersNotice(`Sign ${provider.label} in again on ${name} to run agent jobs there.`);
    if (focus.connect) {
      dropConnectParam();
      const runner = runnersView.runners.find((item) => item.name === name);
      const active = (runner?.logins || []).find(
        (login) => login.provider === provider.id && RUNNER_LOGIN_ACTIVE.has(login.state));
      if (active) {
        attachRunnerLogin(name, provider.id, active.id);
      } else if (runner?.status === "active" && button && !button.disabled) {
        void startRunnerLogin(name, provider.id);
      }
    }
  }

  async function startRunnerLogin(runner, provider) {
    setRunnersNotice("");
    try {
      const payload = await runnersRequest(
        "POST", `/api/runners/${encodeURIComponent(runner)}/login`, { provider });
      attachRunnerLogin(runner, provider, payload.id);
    } catch (error) {
      if (error.code === "login_active" && error.payload?.id) {
        attachRunnerLogin(runner, provider, error.payload.id);
        return;
      }
      setRunnersNotice(`Could not start the sign-in: ${error.message}`, true);
    }
  }

  // Terminal bytes -> readable text: escape sequences dropped, carriage
  // returns applied per line. Minimal on purpose; a sign-in screen is a few
  // lines of text and a URL.
  function runnerTerminalText(raw) {
    const stripped = raw
      .replace(/\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)/g, "")
      .replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, "")
      .replace(/\x1b[@-Z\\-_]/g, "")
      .replace(/\r\n/g, "\n");
    return stripped
      .split("\n")
      .map((line) => {
        const parts = line.split("\r");
        const visible = parts.reverse().find((part) => part.length) || "";
        return visible.replace(/[\x00-\x08\x0b-\x1f\x7f]/g, "");
      })
      .join("\n");
  }

  function runnerLoginUrls(text) {
    const seen = new Set();
    for (const match of text.matchAll(/https:\/\/[^\s"'<>]+/g)) {
      try {
        const url = new URL(match[0].replace(/[.,;)]+$/, ""));
        if (url.protocol === "https:") seen.add(url.href);
      } catch (_error) {
        // not a URL
      }
    }
    return Array.from(seen);
  }

  function renderRunnerLogin() {
    const login = runnersView.login;
    const panel = $("[data-runner-login]");
    if (!panel) return;
    panel.hidden = !login;
    if (!login) return;
    const provider = RUNNER_PROVIDERS.find((item) => item.id === login.provider);
    $("[data-runner-login-title]").textContent =
      `Sign in ${provider?.label || login.provider} on ${login.runner}`;
    $("[data-runner-login-status]").textContent =
      (RUNNER_LOGIN_STATES[login.state] || login.state) + (login.message ? ` ${login.message}` : "");
    const text = runnerTerminalText(login.text);
    const output = $("[data-runner-login-output]");
    const atBottom = output.scrollTop + output.clientHeight >= output.scrollHeight - 8;
    output.textContent = text;
    if (atBottom) output.scrollTop = output.scrollHeight;
    const links = $("[data-runner-login-links]");
    links.replaceChildren();
    for (const href of runnerLoginUrls(text)) {
      const anchor = runnersEl("a", "text-sm text-primary underline break-all", href);
      anchor.href = href;
      anchor.target = "_blank";
      anchor.rel = "noopener noreferrer";
      anchor.setAttribute("referrerpolicy", "no-referrer");
      links.append(anchor);
    }
    const active = RUNNER_LOGIN_ACTIVE.has(login.state);
    $("[data-runner-login-form]").hidden = !active;
    $("[data-runner-login-cancel]").hidden = !active;
    $("[data-runner-login-close]").hidden = active;
  }

  function stopRunnerLoginPolling() {
    const login = runnersView.login;
    if (login?.timer) window.clearTimeout(login.timer);
    if (login) login.timer = 0;
  }

  function attachRunnerLogin(runner, provider, id) {
    stopRunnerLoginPolling();
    runnersView.login = {
      runner, provider, id, seq: 0, state: "pending", message: "", text: "",
      timer: 0, decoder: new TextDecoder("utf-8"),
    };
    renderRunnerLogin();
    $("[data-runner-login]")?.scrollIntoView({ block: "nearest" });
    void pollRunnerLogin(runnersView.login);
  }

  async function pollRunnerLogin(login) {
    if (runnersView.login !== login) return;
    try {
      const payload = await runnersRequest("GET",
        `/api/runners/${encodeURIComponent(login.runner)}/login/${encodeURIComponent(login.id)}/output?after=${login.seq}`);
      if (runnersView.login !== login) return;
      if (payload.truncated) login.text += "\n[earlier output was trimmed]\n";
      for (const chunk of payload.chunks || []) {
        const bytes = Uint8Array.from(atob(chunk.data), (c) => c.charCodeAt(0));
        login.text += login.decoder.decode(bytes, { stream: true });
        login.seq = Math.max(login.seq, Number(chunk.seq) || 0);
      }
      if (login.text.length > RUNNER_LOGIN_MAX_TEXT) {
        login.text = login.text.slice(-RUNNER_LOGIN_MAX_TEXT);
      }
      login.state = payload.state || login.state;
      login.message = RUNNER_LOGIN_ACTIVE.has(login.state) ? "" : (payload.message || "");
      renderRunnerLogin();
      if (!RUNNER_LOGIN_ACTIVE.has(login.state) && !payload.more) {
        void loadRunners();
        return;
      }
      login.timer = window.setTimeout(() => { void pollRunnerLogin(login); },
        payload.more ? 0 : RUNNER_LOGIN_POLL_MS);
    } catch (error) {
      if (runnersView.login !== login) return;
      login.message = `(${error.message}; retrying)`;
      renderRunnerLogin();
      login.timer = window.setTimeout(() => { void pollRunnerLogin(login); }, RUNNER_LOGIN_POLL_MS * 5);
    }
  }

  async function sendRunnerLoginInput() {
    const login = runnersView.login;
    const input = $("[data-runner-login-input]");
    if (!login || !input) return;
    const value = input.value.trim();
    if (!value) return;
    try {
      await runnersRequest("POST",
        `/api/runners/${encodeURIComponent(login.runner)}/login/${encodeURIComponent(login.id)}/input`,
        { data: value + "\r" });
      input.value = "";
    } catch (error) {
      login.message = `(Could not send: ${error.message})`;
      renderRunnerLogin();
    }
  }

  async function cancelRunnerLogin() {
    const login = runnersView.login;
    if (!login) return;
    try {
      const payload = await runnersRequest("POST",
        `/api/runners/${encodeURIComponent(login.runner)}/login/${encodeURIComponent(login.id)}/cancel`, {});
      login.state = payload.state || "cancelled";
      renderRunnerLogin();
    } catch (error) {
      login.message = `(Could not cancel: ${error.message})`;
      renderRunnerLogin();
    }
  }

  const RUNNER_SETUP_POLL_MS = 4000;
  const RUNNER_SSH_POLL_MS = 2000;
  const runnerSetup = {
    token: null,
    tokenTimer: 0,
    ssh: null,
    sshTimer: 0,
    expiryTimer: 0,
  };

  function runnerSetupStatus(message, tone) {
    const node = $("[data-runner-setup-status]");
    if (!node) return;
    node.textContent = message || "";
    node.classList.toggle("text-red-500", tone === "bad");
    node.classList.toggle("text-emerald-500", tone === "good");
    node.classList.toggle("text-muted-foreground", tone !== "bad" && tone !== "good");
  }

  function runnerSetupDefaultName() {
    const base = String(state.session?.nodeName || "my").toLowerCase().replace(/[^a-z0-9-]+/g, "-")
      .replace(/^-+|-+$/g, "").replace(/^[^a-z]+/, "") || "my";
    const taken = new Set(runnersView.runners.map((runner) => runner.name));
    for (let index = 1; index < 100; index += 1) {
      const name = `${base}-runner${index === 1 ? "" : `-${index}`}`.slice(0, 63).replace(/-+$/, "");
      if (!taken.has(name)) return name;
    }
    return `${base}-runner`;
  }

  function runnerSetupName() {
    const input = $("[data-runner-setup-name]");
    const name = String(input?.value || "").trim().toLowerCase();
    if (!/^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(name)) {
      runnerSetupStatus("Pick a runner name: lowercase letters, digits and dashes, starting with a letter.", "bad");
      input?.focus();
      return "";
    }
    return name;
  }

  function openRunnerSetup(open) {
    const panel = $("[data-runner-setup]");
    const toggle = $("[data-runner-setup-open]");
    if (!panel) return;
    panel.hidden = !open;
    toggle?.setAttribute("aria-expanded", String(Boolean(open)));
    if (!open) return;
    const input = $("[data-runner-setup-name]");
    if (input && !input.value) {
      input.value = runnerSetupDefaultName();
      input.dataset.auto = "1";
    }
    panel.scrollIntoView({ block: "nearest" });
    input?.focus();
  }

  function selectRunnerSetupTab(tab) {
    for (const button of $$("[data-runner-setup-tab]")) {
      const on = button.dataset.runnerSetupTab === tab;
      button.setAttribute("aria-selected", String(on));
      button.classList.toggle("text-foreground", on);
      button.classList.toggle("text-muted-foreground", !on);
    }
    for (const pane of $$("[data-runner-setup-pane]")) {
      pane.hidden = pane.dataset.runnerSetupPane !== tab;
    }
  }

  function runnerSetupExpiryLabel(expiresAt) {
    const left = expiresAt - Date.now();
    if (left <= 0) return "Expired. Make a new command.";
    const minutes = Math.ceil(left / 60000);
    return `Works once, for ${minutes} more minute${minutes === 1 ? "" : "s"}.`;
  }

  function stopRunnerSetupTimers() {
    window.clearTimeout(runnerSetup.tokenTimer);
    window.clearTimeout(runnerSetup.sshTimer);
    window.clearTimeout(runnerSetup.expiryTimer);
    runnerSetup.tokenTimer = 0;
    runnerSetup.sshTimer = 0;
    runnerSetup.expiryTimer = 0;
  }

  async function mintRunnerSetupToken(via) {
    const name = runnerSetupName();
    if (!name) return null;
    runnerSetupStatus("Making a one-time command…");
    try {
      const payload = await runnersRequest("POST", "/api/runners/setup-tokens", { name, via });
      runnerSetupStatus("");
      return payload;
    } catch (error) {
      runnerSetupStatus(`Could not make the command: ${error.message}`, "bad");
      return null;
    }
  }

  async function showRunnerSetupCommand() {
    const minted = await mintRunnerSetupToken("command");
    if (!minted) return;
    stopRunnerSetupTimers();
    runnerSetup.token = { id: minted.id, runner: minted.runner, expiresAt: minted.expiresAt };
    const box = $("[data-runner-setup-command-box]");
    const pre = $("[data-runner-setup-command]");
    if (pre) pre.textContent = minted.command;
    if (box) box.hidden = false;
    const expiry = $("[data-runner-setup-expiry]");
    const tick = () => {
      if (!runnerSetup.token) return;
      if (expiry) expiry.textContent = runnerSetupExpiryLabel(runnerSetup.token.expiresAt);
      runnerSetup.expiryTimer = window.setTimeout(tick, 15000);
    };
    tick();
    runnerSetupStatus(`Waiting for ${minted.runner} to register. Run the command on the machine now.`);
    pollRunnerSetupToken();
  }

  function clearRunnerSetupCommand() {
    const pre = $("[data-runner-setup-command]");
    if (pre) pre.textContent = "";
    const box = $("[data-runner-setup-command-box]");
    if (box) box.hidden = true;
  }

  function pollRunnerSetupToken() {
    const token = runnerSetup.token;
    if (!token) return;
    runnerSetup.tokenTimer = window.setTimeout(async () => {
      if (runnerSetup.token !== token) return;
      try {
        const payload = await runnersRequest("GET", `/api/runners/setup-tokens/${encodeURIComponent(token.id)}`);
        const setup = payload.setup || {};
        if (setup.state === "used") {
          runnerSetup.token = null;
          stopRunnerSetupTimers();
          clearRunnerSetupCommand();
          runnerSetupStatus(`${setup.runner} registered. Sign its agent CLI in with Connect Claude or Connect Codex below.`, "good");
          await loadRunners();
          const card = $$("[data-runner]").find((node) => node.dataset.runner === setup.runner);
          card?.classList.add("ring-2", "ring-primary");
          card?.scrollIntoView({ block: "center" });
          return;
        }
        if (setup.state === "expired" || setup.state === "revoked") {
          runnerSetup.token = null;
          stopRunnerSetupTimers();
          clearRunnerSetupCommand();
          runnerSetupStatus(setup.state === "expired"
            ? "The command expired before a runner registered. Make a new one."
            : "The command was cancelled.", setup.state === "expired" ? "bad" : undefined);
          return;
        }
      } catch (_error) {
      }
      pollRunnerSetupToken();
    }, RUNNER_SETUP_POLL_MS);
  }

  async function revokeRunnerSetupToken() {
    const token = runnerSetup.token;
    if (!token) return;
    runnerSetup.token = null;
    stopRunnerSetupTimers();
    clearRunnerSetupCommand();
    try {
      await runnersRequest("POST", `/api/runners/setup-tokens/${encodeURIComponent(token.id)}/revoke`, {});
      runnerSetupStatus("The command was cancelled. It can no longer register a runner.");
    } catch (error) {
      runnerSetupStatus(`Could not cancel the command: ${error.message}`, "bad");
    }
  }

  async function copyRunnerSetupCommand() {
    const text = $("[data-runner-setup-command]")?.textContent || "";
    if (!text) return;
    try {
      await navigator.clipboard.writeText(text);
      runnerSetupStatus("Copied. Paste it into a terminal on the machine.", "good");
    } catch (_error) {
      runnerSetupStatus("Select the command and copy it.", "bad");
    }
  }

  async function openRunnerSetupInDesktop() {
    const host = String($("[data-runner-ssh-host]")?.value || "").trim();
    const user = String($("[data-runner-ssh-user]")?.value || "root").trim() || "root";
    if (host && !/^(?:[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?|(?:[0-9]{1,3}\.){3}[0-9]{1,3})$/.test(host)) {
      runnerSetupStatus("Enter the server as a host name or IPv4 address.", "bad");
      return;
    }
    const minted = await mintRunnerSetupToken("desktop");
    if (!minted) return;
    const url = new URL("forkmesh://control/cloudflare");
    url.searchParams.set("mode", "ssh");
    url.searchParams.set("role", "runner");
    if (host) url.searchParams.set("vpsHost", host);
    url.searchParams.set("vpsUser", user);
    url.searchParams.set("node", minted.runner);
    url.searchParams.set("runnerToken", minted.token);
    url.searchParams.set("app", minted.appUrl);
    stopRunnerSetupTimers();
    runnerSetup.token = { id: minted.id, runner: minted.runner, expiresAt: minted.expiresAt };
    runnerSetupStatus(`Opening ForkMesh Desktop → Hosts. Click Install there; it signs in over SSH from your computer. Waiting for ${minted.runner} to register…`);
    pollRunnerSetupToken();
    window.location.href = url.href;
  }

  function runnerSshAuthKind() {
    return $$("[data-runner-ssh-auth]").find((input) => input.checked)?.value === "key" ? "key" : "password";
  }

  function syncRunnerSshAuth() {
    const key = runnerSshAuthKind() === "key";
    const passwordRow = $("[data-runner-ssh-password-row]");
    const keyRow = $("[data-runner-ssh-key-row]");
    if (passwordRow) passwordRow.hidden = key;
    if (keyRow) keyRow.hidden = !key;
  }

  function clearRunnerSshSecrets() {
    for (const selector of ["[data-runner-ssh-password]", "[data-runner-ssh-key]", "[data-runner-ssh-passphrase]"]) {
      const field = $(selector);
      if (field) field.value = "";
    }
  }

  async function probeRunnerSsh() {
    const host = String($("[data-runner-ssh-host]")?.value || "").trim();
    const port = Number($("[data-runner-ssh-port]")?.value || 22);
    if (!host) {
      runnerSetupStatus("Enter the server's host name or IP address.", "bad");
      return;
    }
    runnerSetupStatus(`Asking ${host} for its SSH host key…`);
    const verify = $("[data-runner-ssh-verify]");
    try {
      const payload = await runnersRequest("POST", "/api/runners/ssh/probe", { host, port });
      runnerSetup.ssh = { id: payload.id, address: payload.address, port: payload.port };
      const keys = $("[data-runner-ssh-keys]");
      if (keys) {
        keys.replaceChildren();
        (payload.hostKeys || []).forEach((key, index) => {
          const label = runnersEl("label", "flex flex-wrap items-center gap-2 text-xs text-foreground");
          const radio = document.createElement("input");
          radio.type = "radio";
          radio.name = "runnerSshFingerprint";
          radio.value = key.fingerprint;
          radio.checked = index === 0;
          radio.dataset.runnerSshFingerprint = "";
          label.append(radio, runnersEl("span", "font-semibold", key.type),
            runnersEl("code", "font-mono break-all", key.fingerprint));
          keys.append(label);
        });
        keys.prepend(runnersEl("p", "text-xs text-muted-foreground",
          `${payload.host} (${payload.address}, port ${payload.port}) answered with:`));
      }
      const installs = $("[data-runner-ssh-installs]");
      if (installs) {
        installs.replaceChildren(...(payload.installs || []).map((line) => runnersEl("li", "", line)));
      }
      const trust = $("[data-runner-ssh-trust]");
      if (trust) trust.checked = false;
      if (verify) verify.hidden = false;
      runnerSetupStatus("Check the fingerprint, then sign in to install.");
    } catch (error) {
      if (verify) verify.hidden = true;
      runnerSetup.ssh = null;
      runnerSetupStatus(error.code === "ssh_setup_unavailable"
        ? "ForkMesh cannot connect for you here. Use ForkMesh Desktop, or the one command."
        : `Could not check the server: ${error.message}`, "bad");
    }
  }

  async function installRunnerSsh(event) {
    event?.preventDefault();
    const probe = runnerSetup.ssh;
    if (!probe) {
      runnerSetupStatus("Check the host key first.", "bad");
      return;
    }
    if (!$("[data-runner-ssh-trust]")?.checked) {
      runnerSetupStatus("Confirm that the fingerprint is your server's host key.", "bad");
      return;
    }
    const name = runnerSetupName();
    if (!name) return;
    const fingerprint = $$("[data-runner-ssh-fingerprint]").find((input) => input.checked)?.value || "";
    const body = {
      id: probe.id,
      fingerprint,
      name,
      username: String($("[data-runner-ssh-user]")?.value || "").trim(),
    };
    if (runnerSshAuthKind() === "key") {
      body.privateKey = String($("[data-runner-ssh-key]")?.value || "");
      body.passphrase = String($("[data-runner-ssh-passphrase]")?.value || "");
    } else {
      body.password = String($("[data-runner-ssh-password]")?.value || "");
    }
    clearRunnerSshSecrets();
    if (!body.password && !body.privateKey) {
      runnerSetupStatus("Enter the password or private key.", "bad");
      return;
    }
    runnerSetupStatus(`Connecting to ${probe.address}…`);
    let payload;
    try {
      payload = await runnersRequest("POST", "/api/runners/ssh/install", body);
    } catch (error) {
      runnerSetupStatus(`Could not start the install: ${error.message}`, "bad");
      return;
    } finally {
      body.password = "";
      body.privateKey = "";
      body.passphrase = "";
    }
    const verify = $("[data-runner-ssh-verify]");
    if (verify) verify.hidden = true;
    runnerSetup.ssh = null;
    const log = $("[data-runner-ssh-log]");
    if (log) {
      log.hidden = false;
      log.textContent = "";
    }
    runnerSetupStatus(`Installing ${payload.runner} on ${probe.address}. This takes a few minutes.`);
    pollRunnerSsh(payload.id);
  }

  function pollRunnerSsh(id) {
    window.clearTimeout(runnerSetup.sshTimer);
    runnerSetup.sshTimer = window.setTimeout(async () => {
      try {
        const payload = await runnersRequest("GET", `/api/runners/ssh/${encodeURIComponent(id)}`);
        const setup = payload.setup || {};
        const log = $("[data-runner-ssh-log]");
        if (log) {
          const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 8;
          log.textContent = setup.log || "";
          if (atBottom) log.scrollTop = log.scrollHeight;
        }
        if (setup.state === "succeeded") {
          runnerSetupStatus(`${setup.runner} is installed${setup.registered ? " and registered" : ""}. Sign its agent CLI in with Connect Claude or Connect Codex below.`, "good");
          await loadRunners();
          return;
        }
        if (setup.state === "failed") {
          runnerSetupStatus(`The install did not finish: ${setup.message || "see the log above"}`, "bad");
          return;
        }
      } catch (_error) {
      }
      pollRunnerSsh(id);
    }, RUNNER_SSH_POLL_MS);
  }

  function initRunnerSetup() {
    $("[data-runner-setup-open]")?.addEventListener("click", () => {
      openRunnerSetup($("[data-runner-setup]")?.hidden !== false);
    });
    $("[data-runner-setup-close]")?.addEventListener("click", () => openRunnerSetup(false));
    $("[data-runner-setup-name]")?.addEventListener("input", (event) => {
      delete event.currentTarget.dataset.auto;
    });
    for (const tab of $$("[data-runner-setup-tab]")) {
      tab.addEventListener("click", () => selectRunnerSetupTab(tab.dataset.runnerSetupTab));
    }
    $("[data-runner-setup-mint]")?.addEventListener("click", () => { void showRunnerSetupCommand(); });
    $("[data-runner-setup-copy]")?.addEventListener("click", () => { void copyRunnerSetupCommand(); });
    $("[data-runner-setup-revoke]")?.addEventListener("click", () => { void revokeRunnerSetupToken(); });
    $("[data-runner-setup-desktop]")?.addEventListener("click", () => { void openRunnerSetupInDesktop(); });
    $("[data-runner-ssh-probe]")?.addEventListener("click", () => { void probeRunnerSsh(); });
    $("[data-runner-ssh-form]")?.addEventListener("submit", (event) => { void installRunnerSsh(event); });
    for (const radio of $$("[data-runner-ssh-auth]")) {
      radio.addEventListener("change", syncRunnerSshAuth);
    }
    window.addEventListener("pagehide", () => {
      clearRunnerSshSecrets();
      stopRunnerSetupTimers();
    });
    if (new URLSearchParams(window.location.search).get("setup") === "1") {
      openRunnerSetup(true);
      dropConnectParam();
    }
  }

  function wireRunnerLoginPanel() {
    const panel = $("[data-runner-login]");
    if (!panel || panel.dataset.wired === "1") return;
    panel.dataset.wired = "1";
    $("[data-runner-login-form]")?.addEventListener("submit", (event) => {
      event.preventDefault();
      void sendRunnerLoginInput();
    });
    $("[data-runner-login-cancel]")?.addEventListener("click", () => { void cancelRunnerLogin(); });
    $("[data-runner-login-close]")?.addEventListener("click", () => {
      stopRunnerLoginPolling();
      runnersView.login = null;
      renderRunnerLogin();
    });
    window.addEventListener("pagehide", () => {
      stopRunnerLoginPolling();
      if (runnersView.refreshTimer) window.clearTimeout(runnersView.refreshTimer);
      runnersView.refreshTimer = 0;
    });
  }

  function initRunnersPage() {
    if (!$("[data-runners-panel]")) return;
    initRunnerSetup();
    $("[data-runners-refresh]")?.addEventListener("click", () => { void loadRunners(); });
    wireRunnerLoginPanel();
    runnersView.focus = requestedRunnerFocus();
    void loadRunners();
  }

  function initSettingsRunners() {
    if (!$("[data-runners-list]")) return;
    wireRunnerLoginPanel();
    const refresh = $("[data-settings-runners-refresh]");
    if (refresh && refresh.dataset.wired !== "1") {
      refresh.dataset.wired = "1";
      refresh.addEventListener("click", () => { void loadRunners(); });
    }
    void loadRunners();
  }

  const RUNNER_ACCOUNTS_PICKER_MAX_POLLS = 30;
  const runnerAccountsPicker = { runners: [], timer: 0, polls: 0, posted: new Set(), notice: "", error: false };

  function runnerAccountsPickerRows(runners) {
    const rows = [];
    for (const runner of runners || []) {
      if (runner?.status !== "active") continue;
      for (const provider of RUNNER_PROVIDERS) {
        const entry = runner.availability?.[provider.id] || {};
        const accounts = Array.isArray(entry.accounts) ? entry.accounts : [];
        if (!entry.binaryFound && !accounts.length) continue;
        const requests = (runner.accountRequests || []).filter((request) => request.provider === provider.id);
        rows.push({
          runner,
          provider,
          accounts,
          pending: requests.find((request) => request.state === "pending") || null,
          failed: requests.find((request) => request.state === "failed" && runnerAccountsPicker.posted.has(request.id)) || null,
        });
      }
    }
    return rows;
  }

  function runnerAccountOptionLabel(account) {
    const parts = [account.label || account.email || "Account"];
    if (account.plan) parts.push(account.plan);
    for (const window of account.usage?.windows || []) {
      parts.push(`${window.label || window.id} ${Math.round(Number(window.usedPercent) || 0)}%`);
    }
    return parts.join(" · ");
  }

  function setRunnerAccountsPickerNotice(message, isError) {
    runnerAccountsPicker.notice = message || "";
    runnerAccountsPicker.error = Boolean(isError);
    const notice = $("[data-runner-accounts-picker-notice]");
    if (!notice) return;
    notice.hidden = !runnerAccountsPicker.notice;
    notice.textContent = runnerAccountsPicker.notice;
    notice.classList.toggle("text-red-500", runnerAccountsPicker.error);
    notice.classList.toggle("text-muted-foreground", !runnerAccountsPicker.error);
  }

  function renderRunnerAccountsPicker() {
    const box = $("[data-runner-accounts-picker]");
    const list = $("[data-runner-accounts-picker-list]");
    if (!box || !list) return;
    const rows = runnerAccountsPickerRows(runnerAccountsPicker.runners);
    box.hidden = !rows.length;
    list.replaceChildren();
    for (const row of rows) {
      const line = runnersEl("div", "flex flex-wrap items-center gap-2");
      line.dataset.runnerAccountsPickerRow = `${row.runner.name}:${row.provider.id}`;
      line.append(runnersEl("span", "font-mono text-xs font-semibold text-foreground", row.runner.name));
      line.append(runnersEl("span", "text-xs text-muted-foreground", row.provider.label));
      if (row.accounts.length) {
        const select = runnersEl("select", "h-8 max-w-full truncate rounded-md border border-border bg-background px-2 text-xs text-foreground");
        select.dataset.runnerAccountSelect = `${row.runner.name}:${row.provider.id}`;
        select.setAttribute("aria-label", `${row.provider.label} account on ${row.runner.name}`);
        for (const account of row.accounts) {
          const option = runnersEl("option", "", runnerAccountOptionLabel(account));
          option.value = account.id;
          option.selected = Boolean(account.active);
          select.append(option);
        }
        select.disabled = Boolean(row.pending);
        select.addEventListener("change", () => {
          const account = row.accounts.find((entry) => entry.id === select.value);
          if (account && !account.active) void switchRunnerAccountFromPicker(row.runner.name, row.provider, account);
        });
        line.append(select);
      } else {
        line.append(runnersEl("span", "text-xs text-muted-foreground", "Not signed in"));
      }
      if (row.pending) {
        line.append(runnersEl("span", "text-xs text-muted-foreground",
          row.pending.action === "remove" ? "Removing..." : "Switching..."));
      } else if (row.failed?.message) {
        line.append(runnersEl("span", "text-xs text-red-500", `Could not switch: ${row.failed.message}`));
      }
      const link = runnersEl("a",
        "ml-auto inline-flex items-center rounded-md border border-border px-2.5 py-1 text-xs font-semibold text-muted-foreground hover:bg-secondary hover:text-foreground transition-colors",
        row.accounts.length ? "Add account" : row.provider.button);
      link.dataset.runnerAccountsPickerConnect = `${row.runner.name}:${row.provider.id}`;
      link.href = `/dashboard/runners?runner=${encodeURIComponent(row.runner.name)}&provider=${encodeURIComponent(row.provider.id)}&connect=1`;
      line.append(link);
      list.append(line);
    }
    setRunnerAccountsPickerNotice(runnerAccountsPicker.notice, runnerAccountsPicker.error);
  }

  function scheduleRunnerAccountsPicker() {
    window.clearTimeout(runnerAccountsPicker.timer);
    runnerAccountsPicker.timer = 0;
    const waiting = runnerAccountsPicker.runners.some((runner) =>
      (runner.accountRequests || []).some((request) => request.state === "pending"));
    if (!waiting || runnerAccountsPicker.polls <= 0 || !$("[data-runner-accounts-picker]")) return;
    runnerAccountsPicker.polls -= 1;
    runnerAccountsPicker.timer = window.setTimeout(() => { void loadRunnerAccountsPicker(); }, RUNNER_ACCOUNT_POLL_MS);
  }

  async function loadRunnerAccountsPicker(fresh) {
    if (!$("[data-runner-accounts-picker]")) return;
    if (fresh) runnerAccountsPicker.polls = RUNNER_ACCOUNTS_PICKER_MAX_POLLS;
    try {
      const payload = await runnersRequest("GET", "/api/runners");
      runnerAccountsPicker.runners = Array.isArray(payload.runners) ? payload.runners : [];
    } catch (error) {
      runnerAccountsPicker.runners = [];
    }
    renderRunnerAccountsPicker();
    scheduleRunnerAccountsPicker();
  }

  async function switchRunnerAccountFromPicker(runner, provider, account) {
    const name = account.label || account.email || "this account";
    try {
      const payload = await runnersRequest("POST", `/api/runners/${encodeURIComponent(runner)}/accounts`,
        { provider: provider.id, account: account.id, action: "activate" });
      if (payload.id) runnerAccountsPicker.posted.add(String(payload.id));
      setRunnerAccountsPickerNotice(`Switching ${runner} to ${name} for ${provider.label}. The runner applies it on its next check-in.`, false);
    } catch (error) {
      setRunnerAccountsPickerNotice(`Could not switch the ${provider.label} account on ${runner}: ${error.message}`, true);
    }
    await loadRunnerAccountsPicker(true);
  }

  // The sidebar's admin-only links follow the session.
  function renderAdminOnlyNavigation(session) {
    for (const link of $$("[data-admin-only]")) {
      link.classList.toggle("hidden", session?.isAdmin !== true);
    }
    startRunnerAttention(session);
  }

  const RUNNER_ATTENTION_POLL_MS = 5 * 60 * 1000;
  const runnerAttention = { started: false, timer: 0, pollMs: RUNNER_ATTENTION_POLL_MS, at: 0 };

  function runnerAttentionSummary(payload) {
    const items = (Array.isArray(payload?.items) ? payload.items : []).filter((item) =>
      item && /^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(String(item.runner || "")) &&
      RUNNER_PROVIDERS.some((entry) => entry.id === item.provider));
    if (!items.length) return null;
    const first = items[0];
    const short = first.provider === "codex" ? "Codex" : "Claude";
    const text = first.state === "expired"
      ? `${first.runner} needs ${short} sign-in`
      : `${first.runner}: ${short} sign-in expires soon`;
    const href = String(first.connectUrl || "").startsWith("/dashboard/runners?")
      ? first.connectUrl
      : `/dashboard/runners?runner=${encodeURIComponent(first.runner)}&provider=${encodeURIComponent(first.provider)}&connect=1`;
    return {
      text: items.length > 1 ? `${text} · +${items.length - 1}` : text,
      state: items.some((item) => item.state === "expired") ? "expired" : "expiring",
      href,
      runner: first.runner,
      provider: first.provider,
    };
  }

  function renderRunnerAttention(summary) {
    const chip = $("[data-runner-attention]");
    if (!chip) return;
    chip.classList.toggle("hidden", !summary);
    chip.classList.toggle("inline-flex", Boolean(summary));
    if (!summary) return;
    chip.dataset.state = summary.state;
    chip.href = summary.href;
    chip.classList.toggle("bg-red-500/10", summary.state === "expired");
    chip.classList.toggle("text-red-500", summary.state === "expired");
    chip.classList.toggle("bg-amber-500/10", summary.state !== "expired");
    chip.classList.toggle("text-amber-600", summary.state !== "expired");
    chip.title = `${summary.text}. Open the sign-in.`;
    chip.setAttribute("aria-label", `${summary.text}. Open the sign-in.`);
    const text = chip.querySelector("[data-runner-attention-text]");
    if (text) text.textContent = summary.text;
    chip.onclick = (event) => {
      if (!$("[data-runners-panel]")) return;
      event.preventDefault();
      const provider = RUNNER_PROVIDERS.find((entry) => entry.id === summary.provider);
      runnersView.focus = { runner: summary.runner, provider: provider.id, connect: true };
      focusRequestedRunner();
    };
  }

  async function loadRunnerAttention() {
    runnerAttention.at = Date.now();
    try {
      const payload = await runnersRequest("GET", "/api/runners/attention");
      const poll = Number(payload.pollMs) || 0;
      runnerAttention.pollMs = poll >= 60000 && poll <= 3600000 ? poll : RUNNER_ATTENTION_POLL_MS;
      renderRunnerAttention(runnerAttentionSummary(payload));
    } catch (error) {
      if (error.status === 401) renderRunnerAttention(null);
    }
  }

  function scheduleRunnerAttention() {
    window.clearTimeout(runnerAttention.timer);
    runnerAttention.timer = window.setTimeout(async () => {
      if (document.visibilityState !== "hidden") await loadRunnerAttention();
      scheduleRunnerAttention();
    }, runnerAttention.pollMs);
  }

  function startRunnerAttention(session) {
    if (!session?.nodeName || runnerAttention.started || !$("[data-runner-attention]")) return;
    runnerAttention.started = true;
    void loadRunnerAttention().finally(scheduleRunnerAttention);
    document.addEventListener("visibilitychange", () => {
      if (document.visibilityState === "visible" && Date.now() - runnerAttention.at > runnerAttention.pollMs) {
        void loadRunnerAttention();
      }
    });
  }

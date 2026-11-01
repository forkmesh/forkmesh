(() => {
  const $ = (selector) => document.querySelector(selector);
  const RETRYABLE = new Set(["push_unavailable", "branch_not_found", "base_not_found", "not_found"]);
  const state = { actor: "", checked: null, repositories: [] };

  function sessionToken() {
    try {
      const value = JSON.parse(localStorage.getItem("forkmesh.session") || "null");
      const token = String(value?.sessionToken || "");
      return token && token !== "cookie" ? token : "";
    } catch (_) {
      return "";
    }
  }

  function withToken(body) {
    const token = sessionToken();
    return token ? { ...body, sessionToken: token } : body;
  }

  function authHeaders() {
    const token = sessionToken();
    return token ? { authorization: `Bearer ${token}` } : {};
  }

  async function api(path, options = {}) {
    const response = await fetch(path, {
      credentials: "same-origin",
      ...options,
      headers: { accept: "application/json", ...authHeaders(), ...(options.headers || {}) },
    });
    const body = await response.json().catch(() => ({}));
    return { status: response.status, body };
  }

  function forumParam() {
    return new URLSearchParams(location.search).get("forum") || "";
  }

  function onForkMesh() {
    return /(^|\.)forkmesh\.com$/.test(location.hostname);
  }

  function launchHref(forum) {
    const base = onForkMesh() ? "/start" : "https://forkmesh.com/start";
    return forum ? `${base}?forum=${encodeURIComponent(forum)}` : base;
  }

  function show(selector, visible) {
    const element = $(selector);
    if (element) element.hidden = !visible;
  }

  function setError(message) {
    const box = $("[data-dm-error]");
    box.textContent = message || "";
    box.hidden = !message;
  }

  function suggestedName(origin) {
    try {
      const host = new URL(origin).hostname.replace(/^(www|forum|community|discuss)\./, "");
      return `${host.split(".")[0].replace(/[^a-z0-9-]+/gi, "-").toLowerCase() || "forum"}-forum`;
    } catch (_) {
      return "forum-mirror";
    }
  }

  function number(value) {
    return Number(value || 0).toLocaleString();
  }

  function renderReport(report) {
    const box = $("[data-dm-report]");
    box.hidden = false;
    box.replaceChildren();
    const head = document.createElement("div");
    if (report.ready) {
      head.className = "dm-ok";
      head.textContent = `Ready to mirror${report.title ? `: ${report.title}` : ""}`;
    } else {
      head.textContent = report.problem || report.message || "This forum cannot be mirrored.";
    }
    box.append(head);
    if (!report.discourse) return;
    const list = document.createElement("dl");
    const hours = Number(report.estimate?.hours || 0);
    [
      ["Categories", number(report.categories)],
      ["Topics", number(report.topics)],
      ["Posts", number(report.posts)],
      ["robots.txt", report.robotsAllow ? "allows copying" : "does not allow copying"],
      ["First copy", `about ${number(report.estimate?.requests)} requests, ${hours < 1 ? "under an hour" : `about ${hours} hours`}, over ${number(report.estimate?.runs)} run${report.estimate?.runs === 1 ? "" : "s"}`],
    ].forEach(([label, value]) => {
      const term = document.createElement("dt");
      term.textContent = label;
      const detail = document.createElement("dd");
      detail.textContent = value;
      list.append(term, detail);
    });
    box.append(list);
  }

  function updateStart() {
    const repoKind = document.querySelector("[data-dm-repo-kind]:checked")?.value || "new";
    const repoChosen = repoKind === "new"
      ? /^[A-Za-z0-9_.-]{1,100}$/.test($("[data-dm-new-name]").value.trim())
      : Boolean($("[data-dm-existing-select]").value);
    $("[data-dm-start]").disabled = !(state.checked?.ready && $("[data-dm-agree]").checked && repoChosen);
  }

  async function runCheck() {
    const origin = $("[data-dm-origin]").value.trim();
    if (!origin) return;
    const button = $("[data-dm-check]");
    button.disabled = true;
    button.textContent = "Checking…";
    state.checked = null;
    try {
      const { body } = await api(`/api/forum-mirrors?origin=${encodeURIComponent(origin)}`);
      state.checked = body.ok ? body : null;
      renderReport(body.ok ? body : { problem: body.message || "The address could not be checked." });
      if (body.ok && body.origin) {
        $("[data-dm-origin]").value = body.origin;
        const name = $("[data-dm-new-name]");
        if (!name.value) name.value = suggestedName(body.origin);
      }
    } catch (_) {
      renderReport({ problem: "The check could not reach this instance." });
    } finally {
      button.disabled = false;
      button.textContent = "Check";
      updateStart();
    }
  }

  async function loadRepositories() {
    const select = $("[data-dm-existing-select]");
    try {
      const { body } = await api("/api/repositories");
      state.repositories = (body.repositories || []).filter((repo) => String(repo.owner || "").toLowerCase() === state.actor);
    } catch (_) {
      state.repositories = [];
    }
    select.replaceChildren();
    if (!state.repositories.length) {
      const option = document.createElement("option");
      option.value = "";
      option.textContent = "No repositories of yours yet";
      select.append(option);
      return;
    }
    state.repositories.forEach((repo) => {
      const option = document.createElement("option");
      option.value = `${repo.owner}/${repo.name}`;
      option.dataset.branch = repo.defaultBranch || repo.branch || "main";
      option.textContent = `${repo.owner}/${repo.name}`;
      select.append(option);
    });
  }

  async function createRepository(name) {
    const { status, body } = await api("/api/hosted-repositories", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(withToken({
        name,
        visibility: "public",
        description: state.checked?.title ? `Mirror of ${state.checked.title}` : "Discourse forum mirror",
        readme: true,
      })),
    });
    if (status >= 300 || !body.repository) throw new Error(body.message || body.error || "The repository could not be created.");
    return { owner: body.repository.owner, repo: body.repository.name, branch: body.repository.defaultBranch || "main" };
  }

  async function startMirror(target) {
    const payload = withToken({
      owner: target.owner,
      repo: target.repo,
      branch: target.branch,
      origin: state.checked.origin,
      uploads: $("[data-dm-uploads]").checked,
      agree: true,
    });
    for (let attempt = 0; attempt < 6; attempt += 1) {
      const { status, body } = await api("/api/forum-mirrors", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify(payload),
      });
      if (status < 300 && body.ok) return body;
      if (!RETRYABLE.has(body.error) || attempt === 5) throw new Error(body.message || body.error || "The mirror could not be started.");
      await new Promise((resolve) => setTimeout(resolve, 3000));
    }
    throw new Error("The mirror could not be started.");
  }

  function renderDone(target, result) {
    const base = `/${encodeURIComponent(target.owner)}/${encodeURIComponent(target.repo)}`;
    $("[data-dm-forum-link]").href = `${base}/forum`;
    $("[data-dm-actions-link]").href = `${base}/actions`;
    const hours = Number(result.estimate?.hours || 0);
    const timing = hours < 1 ? "The first copy takes under an hour." : `The first copy takes about ${hours} hours; topics appear as each half-hourly run finishes.`;
    $("[data-dm-done-text]").textContent = result.dispatched
      ? `The importer and its settings are in ${target.owner}/${target.repo}, and the first run has started. ${timing}`
      : `The importer and its settings are in ${target.owner}/${target.repo}, but this instance does not run Actions yet, so nothing copies until they are turned on.`;
    show("[data-dm-form]", false);
    show("[data-dm-done]", true);
  }

  async function submit(event) {
    event.preventDefault();
    setError("");
    const button = $("[data-dm-start]");
    button.disabled = true;
    button.textContent = "Starting…";
    try {
      let target;
      if ((document.querySelector("[data-dm-repo-kind]:checked")?.value || "new") === "new") {
        target = await createRepository($("[data-dm-new-name]").value.trim());
      } else {
        const option = $("[data-dm-existing-select]").selectedOptions[0];
        const [owner, repo] = String(option?.value || "").split("/");
        target = { owner, repo, branch: option?.dataset.branch || "main" };
      }
      renderDone(target, await startMirror(target));
    } catch (error) {
      setError(String(error?.message || error));
      button.textContent = "Start mirroring";
      updateStart();
    }
  }

  async function init() {
    const forum = forumParam();
    $("[data-dm-launch-link]").href = launchHref(forum);
    $("[data-dm-signin-link]").href = `/login?next=${encodeURIComponent(location.pathname + location.search)}`;
    let who;
    try {
      who = await api("/api/forum-mirrors");
    } catch (_) {
      who = { status: 0, body: {} };
    }
    if (who.status === 401) {
      show("[data-dm-signin]", true);
      return;
    }
    if (!who.body.ok) {
      show("[data-dm-not-admin]", true);
      return;
    }
    state.actor = String(who.body.actor || "").toLowerCase();
    show("[data-dm-form]", true);
    $("[data-dm-check]").addEventListener("click", runCheck);
    $("[data-dm-origin]").addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        runCheck();
      }
    });
    document.querySelectorAll("[data-dm-repo-kind]").forEach((input) => input.addEventListener("change", () => {
      const existing = document.querySelector("[data-dm-repo-kind]:checked")?.value === "existing";
      show("[data-dm-new]", !existing);
      show("[data-dm-existing]", existing);
      updateStart();
    }));
    ["[data-dm-agree]", "[data-dm-new-name]", "[data-dm-existing-select]"].forEach((selector) => {
      $(selector).addEventListener("input", updateStart);
      $(selector).addEventListener("change", updateStart);
    });
    $("[data-dm-form]").addEventListener("submit", submit);
    void loadRepositories();
    if (forum) {
      $("[data-dm-origin]").value = forum;
      void runCheck();
    }
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else void init();
})();

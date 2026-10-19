
  const PULL_BATCH_FILE = ".forkmesh/pull-batch.json";
  const PULL_BATCH_MAX_ROWS = 100;
  const PULL_BATCH_TITLE_MAX = 240;
  const PULL_BATCH_BODY_MAX_BYTES = 60 * 1024;
  const PULL_BATCH_STOP_CODES = new Set([
    "remote_unreachable", "inbox_full", "author_quota", "rate_limited", "bad_signature",
  ]);
  const PULL_BATCH_DEPENDENCY_LINE = /^\s*(?:[-*+]\s+)?(?:\*\*)?depends?\s+on(?:\*\*)?\s*:(?:\*\*)?.*#[1-9].*$/gim;

  function pullBatchCleanTitle(value) {
    return Array.from(String(value ?? "")
      .replace(/[\u0000-\u001f\u007f​-‏‪-‮⁦-⁩﻿]/g, " ")
      .replace(/\s+/g, " ")
      .trim())
      .slice(0, PULL_BATCH_TITLE_MAX)
      .join("")
      .trim();
  }

  function pullBatchCleanBody(value) {
    return String(value ?? "")
      .replace(/\r\n?/g, "\n")
      .replace(/[\u0000-\u0008\u000b-\u001f\u007f‪-‮⁦-⁩﻿]/g, "")
      .replace(/[ \t]+$/gm, "")
      .replace(/^\n+/, "")
      .replace(/\s+$/, "");
  }

  function pullBatchBranchName(value) {
    const name = String(value ?? "").trim().replace(/^refs\/heads\//, "");
    if (!name || name.length > 255) return "";
    if (!/^[A-Za-z0-9._\/-]+$/.test(name)) return "";
    if (/^[-/.]|\/$|\.\.|\/\/|\.lock$|@\{/.test(name)) return "";
    return name;
  }

  function pullBatchDependencyList(value) {
    const values = Array.isArray(value) ? value : value === undefined || value === null || value === "" ? [] : [value];
    const out = [];
    values.slice(0, 20).forEach((item) => {
      const text = String(item ?? "").trim();
      const number = /^#?([1-9]\d{0,8})$/.exec(text);
      const dependency = number ? `#${number[1]}` : pullBatchBranchName(text);
      if (dependency && !out.includes(dependency)) out.push(dependency);
    });
    return out;
  }

  function parsePullBatchFile(text, defaultBranch = "main") {
    let data;
    try {
      data = JSON.parse(String(text || ""));
    } catch (_) {
      return { entries: [], error: `${PULL_BATCH_FILE} is not valid JSON.` };
    }
    const list = Array.isArray(data) ? data : Array.isArray(data?.pulls) ? data.pulls : null;
    if (!list) return { entries: [], error: `${PULL_BATCH_FILE} must be a list of pull requests.` };
    const entries = [];
    const seen = new Set();
    for (const item of list.slice(0, PULL_BATCH_MAX_ROWS)) {
      if (!item || typeof item !== "object") continue;
      const branch = pullBatchBranchName(item.branch || item.head);
      if (!branch || seen.has(branch)) continue;
      seen.add(branch);
      entries.push({
        branch,
        base: pullBatchBranchName(item.base) || defaultBranch,
        title: pullBatchCleanTitle(item.title),
        body: pullBatchCleanBody(item.body ?? item.description),
        dependsOn: pullBatchDependencyList(item.dependsOn ?? item.depends_on).filter((name) => name !== branch),
      });
    }
    return { entries, error: "" };
  }

  function pullBatchTitleFromBranch(branch, prefix) {
    const tail = prefix && branch.startsWith(prefix) ? branch.slice(prefix.length) : branch;
    const words = tail.split(/[\/_-]+/).filter(Boolean).join(" ");
    return words ? words[0].toUpperCase() + words.slice(1) : "";
  }

  function buildPullBatchRows(entries, branchNames, defaultBranch, prefix) {
    const names = new Set(branchNames);
    const rows = [];
    const listed = new Set();
    entries.forEach((entry) => {
      if (prefix && !entry.branch.startsWith(prefix)) return;
      listed.add(entry.branch);
      rows.push({ ...entry, include: true, fromFile: true, missing: !names.has(entry.branch) });
    });
    branchNames
      .filter((name) => name !== defaultBranch && !listed.has(name) && (!prefix || name.startsWith(prefix)))
      .sort((a, b) => a.localeCompare(b))
      .forEach((name) => {
        rows.push({
          branch: name,
          base: defaultBranch,
          title: pullBatchTitleFromBranch(name, prefix),
          body: "",
          dependsOn: [],
          include: true,
          fromFile: false,
          missing: false,
        });
      });
    return rows;
  }

  function orderPullBatchRows(rows) {
    const byBranch = new Map(rows.map((row, index) => [row.branch, { row, index }]));
    const ordered = [];
    const looped = new Set();
    const mark = new Map();
    const visit = (row, trail) => {
      const seen = mark.get(row.branch);
      if (seen === "done") return;
      if (seen === "visiting") {
        trail.slice(trail.indexOf(row.branch)).forEach((name) => looped.add(name));
        return;
      }
      mark.set(row.branch, "visiting");
      row.dependsOn
        .map((name) => byBranch.get(name))
        .filter(Boolean)
        .sort((a, b) => a.index - b.index)
        .forEach((dependency) => visit(dependency.row, [...trail, row.branch]));
      mark.set(row.branch, "done");
      ordered.push(row);
    };
    rows.forEach((row) => visit(row, []));
    return { rows: ordered, looped };
  }

  function pullBatchPlan(rows, sent, outcome = {}) {
    const { rows: ordered, looped } = orderPullBatchRows(rows);
    const byBranch = new Map(ordered.map((row) => [row.branch, row]));
    const plan = new Map();
    const blocked = (row, reason, waitsFor = []) => ({ state: "blocked", reason, waitsFor, numbers: [] });
    ordered.forEach((row) => {
      const done = sent[row.branch];
      if (done?.number) {
        plan.set(row.branch, { state: "sent", number: done.number, waitsFor: [], numbers: [] });
        return;
      }
      if (outcome[row.branch]?.empty) {
        plan.set(row.branch, { state: "empty", waitsFor: [], numbers: [] });
        return;
      }
      if (outcome[row.branch]?.error) {
        plan.set(row.branch, { state: "failed", reason: outcome[row.branch].error, waitsFor: [], numbers: [] });
        return;
      }
      if (!row.include) {
        plan.set(row.branch, { state: "skipped", waitsFor: [], numbers: [] });
        return;
      }
      if (row.missing) {
        plan.set(row.branch, blocked(row, `${row.branch} is not a branch of this repository.`));
        return;
      }
      if (looped.has(row.branch)) {
        plan.set(row.branch, blocked(row, "Its dependencies form a loop."));
        return;
      }
      if (!row.title) {
        plan.set(row.branch, blocked(row, "Needs a title."));
        return;
      }
      if (new TextEncoder().encode(row.body).byteLength > PULL_BATCH_BODY_MAX_BYTES) {
        plan.set(row.branch, blocked(row, "The description is too long."));
        return;
      }
      const numbers = [];
      const waitsFor = [];
      const stuck = [];
      const why = [];
      row.dependsOn.forEach((dependency) => {
        if (dependency.startsWith("#")) {
          numbers.push(Number(dependency.slice(1)));
          return;
        }
        if (sent[dependency]?.number) {
          numbers.push(Number(sent[dependency].number));
          return;
        }
        if (outcome[dependency]?.empty) return;
        const earlier = plan.get(dependency);
        if (byBranch.has(dependency) && earlier && ["ready", "after"].includes(earlier.state)) {
          waitsFor.push(dependency);
          return;
        }
        stuck.push(dependency);
        why.push(!byBranch.has(dependency)
          ? `${dependency}, which is not in this list and has not been sent`
          : earlier?.state === "failed"
            ? `${dependency}, which could not be sent`
            : earlier?.state === "skipped"
              ? `${dependency}, which is not selected`
              : `${dependency}, which cannot be sent yet`);
      });
      if (stuck.length) {
        plan.set(row.branch, blocked(row, `Waits for ${why.join("; ")}.`, stuck));
        return;
      }
      plan.set(row.branch, waitsFor.length
        ? { state: "after", waitsFor, numbers }
        : { state: "ready", waitsFor: [], numbers });
    });
    return { order: ordered, plan };
  }

  function pullBatchBody(body, numbers) {
    const clean = pullBatchCleanBody(body);
    const unique = [...new Set(numbers.map(Number).filter((n) => Number.isInteger(n) && n > 0))];
    if (!unique.length) return clean;
    const refs = unique.map((n) => `#${n}`).join(", ");
    const lead = [
      `Depends on: ${refs}`,
      "",
      `This branch builds on ${refs}, so until ${unique.length === 1 ? "that pull request merges" : "those pull requests merge"} it also carries ${unique.length === 1 ? "its" : "their"} commits. Merge ${refs} first: commits already on the base branch are skipped when this one is merged.`,
    ].join("\n");
    const rest = clean.replace(PULL_BATCH_DEPENDENCY_LINE, "").replace(/^\n+/, "").replace(/\n{3,}/g, "\n\n");
    return rest ? `${lead}\n\n${rest}` : lead;
  }

  function pullBatchStorageKey(repo, remote) {
    return `forkmesh.pullBatch.v1:${location.host}/${repoDisplayKey(repo)}>${String(remote?.url || "").toLowerCase()}`;
  }

  function loadPullBatchSent(repo, remote) {
    try {
      const data = JSON.parse(localStorage.getItem(pullBatchStorageKey(repo, remote)) || "{}");
      const sent = {};
      if (data && typeof data === "object" && !Array.isArray(data)) {
        Object.entries(data).forEach(([branch, value]) => {
          const number = Number(value?.number);
          if (pullBatchBranchName(branch) && Number.isInteger(number) && number > 0) {
            sent[branch] = { number, at: Number(value.at) || 0 };
          }
        });
      }
      return sent;
    } catch (_) {
      return {};
    }
  }

  function savePullBatchSent(repo, remote, sent) {
    try {
      localStorage.setItem(pullBatchStorageKey(repo, remote), JSON.stringify(sent));
      return true;
    } catch (_) {
      return false;
    }
  }

  function remotePullSubmitErrorText(error, remote, base, head) {
    const where = `${remote.owner}/${remote.name} on ${remote.host}`;
    return String(error?.message || "") === "remote_unreachable"
      ? `Nothing was delivered: ${remote.host} could not be reached, or it runs a ForkMesh version that does not take pull requests from other instances yet.`
      : webPullSubmitErrorText(error, base, head, where);
  }

  function pullBatchCompareBase(row) {
    const branches = Array.isArray(row?.dependsOn)
      ? row.dependsOn.filter((dependency) => !String(dependency).startsWith("#"))
      : [];
    return branches.length === 1 ? branches[0] : "";
  }

  async function runPullBatch(batch, { send = submitRemotePullOpen, onChange = () => {} } = {}) {
    const { repo, remote } = batch;
    batch.running = true;
    batch.stopRequested = false;
    batch.notice = "";
    batch.outcome = Object.fromEntries(Object.entries(batch.outcome || {}).filter(([, value]) => value?.empty));
    batch.sending = "";
    let sentNow = 0;
    onChange(batch);
    try {
      for (;;) {
        if (batch.stopRequested) {
          batch.notice = "Stopped. Press Send all to continue with the rest.";
          break;
        }
        const { order, plan } = pullBatchPlan(batch.rows, batch.sent, batch.outcome);
        const row = order.find((candidate) => plan.get(candidate.branch).state === "ready");
        if (!row) break;
        const step = plan.get(row.branch);
        batch.sending = row.branch;
        onChange(batch);
        try {
          const result = await send(repo, remote, row.title, pullBatchBody(row.body, step.numbers), row.base, row.branch, pullBatchCompareBase(row));
          const number = Number(result?.number || 0);
          if (!Number.isInteger(number) || number <= 0) throw new Error("number_unavailable");
          batch.sent = { ...batch.sent, [row.branch]: { number, at: Date.now() } };
          savePullBatchSent(repo, remote, batch.sent);
          sentNow += 1;
        } catch (error) {
          const code = String(error?.code || error?.message || "");
          if (code === "no_changes") {
            batch.outcome[row.branch] = { empty: true };
          } else {
            batch.outcome[row.branch] = { error: remotePullSubmitErrorText(error, remote, row.base, row.branch) };
            if (PULL_BATCH_STOP_CODES.has(code) || Number(error?.status) === 429) {
              batch.notice = `Stopped after ${row.branch}: ${batch.outcome[row.branch].error}`;
              break;
            }
          }
        }
      }
    } finally {
      batch.running = false;
      batch.sending = "";
      onChange(batch);
    }
    return sentNow;
  }

  function pullBatchRouteActive(repo) {
    const parts = repoRouteParts();
    return Boolean(repo) && parts[2] === "pulls" && parts[3] === "send";
  }

  function pullBatchUrl(repo) {
    return `${repoLinkUrl(repo)}/pulls/send`;
  }

  function syncRepoPullBatchEntry(repo = state.selectedRepo) {
    const data = state.repoRemoteForks;
    const show = Boolean(repo && data.canManage && data.forks.length
      && data.key === repoKey(repo).toLowerCase() && !repoReadOnlyMirror(repo));
    $$("[data-repo-pull-batch-open]").forEach((link) => {
      link.classList.toggle("hidden", !show);
      link.classList.toggle("inline-flex", show);
      if (repo) link.setAttribute("href", pullBatchUrl(repo));
    });
  }

  async function loadPullBatchFile(repo, defaultBranch) {
    try {
      const data = await fetchRepoJson(repoLiveUrl(repo, "blob", { path: PULL_BATCH_FILE, ref: defaultBranch }));
      if (typeof data?.content !== "string") return { entries: [], error: "", found: false };
      let text = data.content;
      if (String(data.encoding || "").toLowerCase() === "base64") {
        text = new TextDecoder().decode(Uint8Array.from(atob(text.replace(/\s+/g, "")), (c) => c.charCodeAt(0)));
      }
      return { ...parsePullBatchFile(text, defaultBranch), found: true };
    } catch (_) {
      return { entries: [], error: "", found: false };
    }
  }

  function pullBatchStatusHtml(batch, row, step) {
    const remote = batch.remote;
    if (batch.sending === row.branch) {
      return `<span class="inline-flex items-center gap-1.5 text-foreground">${loadingHtml("Comparing, signing and sending…")}</span>`;
    }
    const link = (number) => `<a href="${escapeHtml(`${remote.url}/pulls/${number}`)}" target="_blank" rel="noopener noreferrer" class="font-medium text-accent hover:underline">#${number}</a>`;
    switch (step.state) {
      case "sent":
        return `<span class="text-success">Sent as</span> ${link(step.number)}`;
      case "empty":
        return `<span class="text-muted-foreground">Nothing to send: ${escapeHtml(row.branch)} has no changes that are not on ${escapeHtml(row.base)}.</span>`;
      case "failed":
        return `<span class="text-destructive">${escapeHtml(step.reason)}</span>`;
      case "skipped":
        return `<span class="text-muted-foreground">Not selected.</span>`;
      case "blocked":
        return `<span class="text-attention">${escapeHtml(step.reason)}</span>`;
      case "after":
        return `<span class="text-muted-foreground">Ready after ${escapeHtml(step.waitsFor.join(", "))} is sent; it will say which pull request it depends on.</span>`;
      default:
        return step.numbers.length
          ? `<span class="text-muted-foreground">Ready. Depends on ${step.numbers.map((n) => link(n)).join(", ")}.</span>`
          : `<span class="text-muted-foreground">Ready.</span>`;
    }
  }

  function pullBatchRowHtml(batch, row, step, index) {
    const id = `pull-batch-${index}`;
    const locked = batch.running || step.state === "sent";
    const dependsOn = row.dependsOn.length
      ? `<span class="text-[11px] text-muted-foreground">after ${escapeHtml(row.dependsOn.join(", "))}</span>`
      : "";
    return `
      <li data-pull-batch-row="${escapeHtml(row.branch)}" class="border-t border-border px-4 py-3 first:border-t-0">
        <div class="flex items-start gap-3">
          <input id="${id}" data-pull-batch-include type="checkbox" ${row.include ? "checked" : ""} ${locked ? "disabled" : ""} aria-label="Send ${escapeHtml(row.branch)}" class="mt-1 h-4 w-4 shrink-0 rounded border-border" />
          <div class="grid min-w-0 flex-1 gap-1">
            <label for="${id}" data-pull-batch-label class="min-w-0 break-words text-sm font-medium text-foreground">${row.title ? escapeHtml(row.title) : '<span class="text-muted-foreground">Untitled</span>'}</label>
            <div class="flex min-w-0 flex-wrap items-center gap-x-2 gap-y-0.5">
              <span class="break-all font-mono text-[11px] text-foreground">${escapeHtml(row.branch)}</span>
              <span class="text-[11px] text-muted-foreground">into ${escapeHtml(row.base)}</span>
              ${dependsOn}
            </div>
            <p data-pull-batch-status class="text-[12px] leading-5">${pullBatchStatusHtml(batch, row, step)}</p>
            <details data-pull-batch-edit class="min-w-0">
              <summary class="cursor-pointer select-none text-[12px] font-medium text-accent">Edit title and description</summary>
              <div class="mt-2 grid gap-2">
                <input data-pull-batch-title type="text" maxlength="${PULL_BATCH_TITLE_MAX}" value="${escapeHtml(row.title)}" ${locked ? "disabled" : ""} aria-label="Title for ${escapeHtml(row.branch)}" class="h-control w-full min-w-0 rounded-control border border-border bg-background px-3 text-[16px] text-foreground outline-none focus:border-accent sm:text-control" />
                <textarea data-pull-batch-body rows="6" ${locked ? "disabled" : ""} aria-label="Description for ${escapeHtml(row.branch)}" class="w-full min-w-0 rounded-control border border-border bg-background px-3 py-2 text-[16px] text-foreground outline-none focus:border-accent sm:text-control">${escapeHtml(row.body)}</textarea>
              </div>
            </details>
          </div>
        </div>
      </li>`;
  }

  function pullBatchSummary(batch, order, plan) {
    const counts = {};
    order.forEach((row) => {
      const key = plan.get(row.branch).state;
      counts[key] = (counts[key] || 0) + 1;
    });
    const ready = (counts.ready || 0) + (counts.after || 0);
    const parts = [`${ready} to send`];
    if (counts.sent) parts.push(`${counts.sent} sent`);
    if (counts.blocked) parts.push(`${counts.blocked} waiting`);
    if (counts.failed) parts.push(`${counts.failed} failed`);
    if (counts.empty) parts.push(`${counts.empty} with nothing to send`);
    return { ready, text: parts.join(" · ") };
  }

  function renderPullBatchRows(batch, { full = true } = {}) {
    const view = document.querySelector("[data-pull-batch]");
    if (!view || view._batch !== batch) return;
    const { order, plan } = pullBatchPlan(batch.rows, batch.sent, batch.outcome);
    const list = view.querySelector("[data-pull-batch-list]");
    const open = new Set(Array.from(view.querySelectorAll("[data-pull-batch-edit][open]"))
      .map((node) => node.closest("[data-pull-batch-row]")?.dataset.pullBatchRow));
    if (list && !full) {
      list.querySelectorAll("[data-pull-batch-row]").forEach((node) => {
        const row = order.find((candidate) => candidate.branch === node.dataset.pullBatchRow);
        if (!row) return;
        const status = node.querySelector("[data-pull-batch-status]");
        if (status) status.innerHTML = pullBatchStatusHtml(batch, row, plan.get(row.branch));
        const label = node.querySelector("[data-pull-batch-label]");
        if (label) label.innerHTML = row.title ? escapeHtml(row.title) : '<span class="text-muted-foreground">Untitled</span>';
      });
    } else if (list) {
      list.innerHTML = order.length
        ? order.map((row, index) => pullBatchRowHtml(batch, row, plan.get(row.branch), index)).join("")
        : `<li class="px-4 py-6 text-center text-sm text-muted-foreground">No branches match${batch.prefix ? ` <span class="font-mono text-foreground">${escapeHtml(batch.prefix)}</span>` : ""}.</li>`;
      list.querySelectorAll("[data-pull-batch-row]").forEach((node) => {
        if (open.has(node.dataset.pullBatchRow)) node.querySelector("[data-pull-batch-edit]")?.setAttribute("open", "");
      });
    }
    const summary = pullBatchSummary(batch, order, plan);
    const line = view.querySelector("[data-pull-batch-summary]");
    if (line) line.textContent = summary.text;
    const notice = view.querySelector("[data-pull-batch-notice]");
    if (notice) {
      notice.textContent = batch.notice || "";
      notice.classList.toggle("hidden", !batch.notice);
    }
    const sendAll = view.querySelector("[data-pull-batch-send]");
    if (sendAll) {
      if (batch.running) {
        setSubmitBusy(sendAll, true, `Sending ${batch.sending || "…"}`);
      } else {
        setSubmitBusy(sendAll, false);
        sendAll.disabled = !summary.ready;
      }
    }
    view.querySelector("[data-pull-batch-stop]")?.classList.toggle("hidden", !batch.running);
    view.querySelectorAll("[data-pull-batch-target], [data-pull-batch-prefix]").forEach((control) => {
      control.disabled = batch.running;
    });
  }

  function pullBatchMessageHtml(repo, text) {
    return `
      <section data-pull-batch-message class="grid gap-3 border-t border-border bg-background p-4">
        <button type="button" data-repo-pull-cancel class="inline-flex h-8 w-fit items-center gap-2 rounded-md border border-border px-3 text-xs font-medium text-foreground hover:bg-secondary"><i data-lucide="arrow-left" class="h-3.5 w-3.5"></i>Back to pull requests</button>
        <p class="text-sm text-muted-foreground">${text}</p>
      </section>`;
  }

  function pullBatchRebuildRows(batch) {
    const names = batch.branches.map((branch) => branch.name);
    const before = new Map((batch.rows || []).map((row) => [row.branch, row]));
    batch.rows = buildPullBatchRows(batch.entries, names, batch.defaultBranch, batch.prefix).map((row) => {
      const kept = before.get(row.branch);
      return kept ? { ...row, title: kept.title, body: kept.body, include: kept.include } : row;
    });
  }

  function pullBatchSelectTarget(batch, url) {
    const remote = batch.forks.find((fork) => fork.url === url) || batch.forks[0];
    batch.remote = remote;
    batch.sent = loadPullBatchSent(batch.repo, remote);
    batch.outcome = {};
    batch.notice = "";
  }

  async function openPullBatch(repo, { navigate = true } = {}) {
    const container = $("[data-repo-pulls]");
    if (!container || !repo) return;
    state.repoCompare = null;
    state.repoRecordDetail = null;
    stopRepoPullActivityRefresh();
    setRepoRecordOpen("pulls", true);
    if (navigate) navigateHistory(pullBatchUrl(repo));
    if (!state.session?.nodeName) {
      container.innerHTML = pullBatchMessageHtml(repo, `<a href="/login?next=${escapeHtml(encodeURIComponent(location.pathname))}" class="font-medium text-accent hover:underline">Sign in</a> as the repository's owner to send pull requests to its forks on other instances.`);
      window.lucide?.createIcons();
      return;
    }
    const current = state.repoPullBatch;
    if (current?.running && repoKey(current.repo) === repoKey(repo)) {
      paintPullBatch(container, current);
      return;
    }
    container.innerHTML = `<div class="border-t border-border px-4 py-6 text-sm text-muted-foreground">${loadingHtml("Reading branches and forks…")}</div>`;
    const token = {};
    state.repoPullBatchLoad = token;
    let branches = [];
    let forks = [];
    try {
      [forks, branches] = await Promise.all([
        loadRepoRemoteForks(repo, { force: true }),
        fetchRepoBranches(repo, { refresh: true }),
      ]);
    } catch (_) {
      branches = [];
    }
    if (state.repoPullBatchLoad !== token || !pullBatchRouteActive(repo)) return;
    if (!state.repoRemoteForks.canManage) {
      container.innerHTML = pullBatchMessageHtml(repo, "Only the repository's owner, or an owner of its organization, can send its branches as pull requests to forks on other instances.");
      window.lucide?.createIcons();
      return;
    }
    if (!forks.length) {
      container.innerHTML = pullBatchMessageHtml(repo, "Save a fork on another instance first: open <span class=\"font-medium text-foreground\">Fork</span> at the top of the repository and add its address under forks on other instances.");
      window.lucide?.createIcons();
      return;
    }
    if (!branches.length) {
      container.innerHTML = pullBatchMessageHtml(repo, "The branch list could not be read from a live mirror. Try again in a moment.");
      window.lucide?.createIcons();
      return;
    }
    const defaultBranch = repoDefaultBranch(repo);
    const file = await loadPullBatchFile(repo, defaultBranch);
    if (state.repoPullBatchLoad !== token || !pullBatchRouteActive(repo)) return;
    const prefix = branches.some((branch) => branch.name.startsWith("pr/")) ? "pr/" : "";
    const batch = {
      repo,
      forks,
      branches,
      defaultBranch,
      entries: file.entries,
      fileError: file.error,
      fileFound: file.found,
      prefix,
      rows: [],
      sent: {},
      outcome: {},
      notice: "",
      running: false,
      sending: "",
    };
    pullBatchSelectTarget(batch, forks[0].url);
    pullBatchRebuildRows(batch);
    state.repoPullBatch = batch;
    paintPullBatch(container, batch);
  }

  function paintPullBatch(container, batch) {
    const repo = batch.repo;
    const fileLine = batch.fileError
      ? `<span class="text-destructive">${escapeHtml(batch.fileError)}</span>`
      : batch.fileFound
        ? `Titles, descriptions and order come from <a href="${escapeHtml(repoLinkUrl(repo, "blob", PULL_BATCH_FILE))}" class="font-mono text-accent hover:underline">${escapeHtml(PULL_BATCH_FILE)}</a> on ${escapeHtml(batch.defaultBranch)}.`
        : `Commit <span class="font-mono text-foreground">${escapeHtml(PULL_BATCH_FILE)}</span> to ${escapeHtml(batch.defaultBranch)} to fill in titles, descriptions and dependencies.`;
    container.innerHTML = `
      <form data-pull-batch class="grid gap-4 border-t border-border bg-background p-4">
        <div class="flex flex-wrap items-center justify-between gap-3">
          <button type="button" data-repo-pull-cancel class="inline-flex h-8 items-center gap-2 rounded-md border border-border px-3 text-xs font-medium text-foreground hover:bg-secondary"><i data-lucide="arrow-left" class="h-3.5 w-3.5"></i>Back to pull requests</button>
        </div>
        <header class="grid gap-1">
          <h2 class="text-[20px] font-semibold leading-8 text-foreground">Send pull requests</h2>
          <p class="text-[13px] text-muted-foreground">Each branch becomes its own pull request on the fork you pick: compared with the branch it depends on (or its base), so it carries only its own commits, then signed in this browser and sent in order. Pull requests already sent to that fork are skipped, so you can press Send all again to finish.</p>
          <p class="text-[12px] text-muted-foreground">${fileLine}</p>
        </header>
        <div class="grid gap-3 sm:grid-cols-2">
          <label class="grid min-w-0 gap-1 text-xs font-medium text-muted-foreground">Send to
            <select data-pull-batch-target class="h-control w-full min-w-0 rounded-control border border-border bg-background px-2 text-[16px] text-foreground outline-none focus:border-accent sm:text-control">${batch.forks.map((fork) => `<option value="${escapeHtml(fork.url)}" ${fork.url === batch.remote.url ? "selected" : ""}>${escapeHtml(`${fork.owner}/${fork.name}`)} on ${escapeHtml(fork.host)}</option>`).join("")}</select>
          </label>
          <label class="grid min-w-0 gap-1 text-xs font-medium text-muted-foreground">Branches starting with
            <input data-pull-batch-prefix type="text" autocomplete="off" autocapitalize="off" spellcheck="false" value="${escapeHtml(batch.prefix)}" placeholder="any branch" class="h-control w-full min-w-0 rounded-control border border-border bg-background px-3 font-mono text-[16px] text-foreground outline-none focus:border-accent sm:text-control" />
          </label>
        </div>
        <div class="overflow-hidden rounded-md border border-border">
          <div class="flex flex-wrap items-center justify-between gap-2 border-b border-border bg-secondary/50 px-4 py-2">
            <span class="text-xs font-medium text-foreground">Branches</span>
            <span data-pull-batch-summary class="text-[11px] text-muted-foreground"></span>
          </div>
          <ol data-pull-batch-list></ol>
        </div>
        <p data-pull-batch-notice role="status" class="hidden text-[12px] text-attention"></p>
        <div class="sticky bottom-0 z-10 flex flex-wrap items-center justify-end gap-2 border-t border-border bg-background py-3">
          ${composeIdentityHtml(state.session, "Signing")}
          <button type="button" data-pull-batch-stop class="hidden h-9 items-center rounded-md border border-border px-3 text-sm font-medium text-foreground hover:bg-secondary">Stop</button>
          <button type="submit" data-pull-batch-send class="inline-flex h-9 flex-1 items-center justify-center gap-2 rounded-md bg-cta px-4 text-sm font-medium text-cta-foreground hover:bg-cta/90 disabled:opacity-50 sm:flex-none"><i data-lucide="send" class="h-4 w-4"></i>Send all</button>
        </div>
      </form>`;
    const view = container.querySelector("[data-pull-batch]");
    view._batch = batch;
    bindPullBatchView(view, batch);
    renderPullBatchRows(batch);
    window.lucide?.createIcons();
  }

  function bindPullBatchView(view, batch) {
    const rowOf = (node) => batch.rows.find((row) => row.branch === node.closest("[data-pull-batch-row]")?.dataset.pullBatchRow);
    view.addEventListener("change", (event) => {
      const target = event.target;
      if (batch.running) return;
      if (target.matches("[data-pull-batch-target]")) {
        pullBatchSelectTarget(batch, target.value);
        renderPullBatchRows(batch);
      } else if (target.matches("[data-pull-batch-include]")) {
        const row = rowOf(target);
        if (row) row.include = target.checked;
        renderPullBatchRows(batch, { full: false });
      } else if (target.matches("[data-pull-batch-title], [data-pull-batch-body]")) {
        renderPullBatchRows(batch, { full: false });
      }
    });
    view.addEventListener("keydown", (event) => {
      if (event.key === "Enter" && event.target.matches("input")) event.preventDefault();
    });
    view.addEventListener("input", (event) => {
      if (batch.running) return;
      if (event.target.matches("[data-pull-batch-prefix]")) {
        batch.prefix = String(event.target.value || "").trim();
        pullBatchRebuildRows(batch);
        renderPullBatchRows(batch);
        return;
      }
      const row = rowOf(event.target);
      if (!row) return;
      if (event.target.matches("[data-pull-batch-title]")) row.title = pullBatchCleanTitle(event.target.value);
      if (event.target.matches("[data-pull-batch-body]")) row.body = pullBatchCleanBody(event.target.value);
    });
    view.addEventListener("click", (event) => {
      if (event.target.closest("[data-pull-batch-stop]")) batch.stopRequested = true;
    });
    view.addEventListener("submit", async (event) => {
      event.preventDefault();
      if (batch.running) return;
      let wakeLock = null;
      try {
        wakeLock = await navigator.wakeLock?.request?.("screen");
      } catch (_) {
        wakeLock = null;
      }
      try {
        await runPullBatch(batch, { onChange: renderPullBatchRows });
      } finally {
        try {
          await wakeLock?.release?.();
        } catch (_) {}
      }
    });
  }

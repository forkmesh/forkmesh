window.ForkMeshDashboard.registerPage("surveys", ({
  select: $, selectAll: $$, session, escapeHtml, relativeTimeLabel,
}) => {
  const STATUS_LABELS = { draft: "Draft", open: "Open", scheduled: "Scheduled", closed: "Closed" };
  const STATUS_TONES = { draft: "", open: " fm-label-success", scheduled: " fm-label-accent", closed: "" };
  const TYPE_LABELS = { single: "Pick one", multiple: "Pick any", text: "Short text", paragraph: "Long text", rating: "Rating" };
  const CHOICE_TYPES = ["single", "multiple"];
  const DEFAULT_SETTINGS = { oneResponsePerPerson: false, opensAt: 0, closesAt: 0, maxResponses: 0, notifyOnResponse: true, notifyOnVisit: true, shuffleOptions: false };
  const TABS = ["results", "responses", "activity"];
  const state = {
    organizations: [], organization: "", surveys: [], summary: {}, detailId: "", detail: null,
    responses: null, tab: "results", questions: [],
  };
  const params = new URL(location.href).searchParams;
  const requested = { org: params.get("org") || "", survey: params.get("survey") || "", tab: TABS.includes(params.get("tab")) ? params.get("tab") : "" };

  async function request(path, method = "GET", body = null) {
    const headers = { accept: "application/json" };
    const token = session()?.sessionToken;
    if (token) headers.authorization = `Bearer ${token}`;
    if (body !== null) headers["content-type"] = "application/json";
    const response = await fetch(path, { method, headers, cache: "no-store",
      body: body === null ? undefined : JSON.stringify(body) });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(errorText(payload, response.status));
      error.status = response.status; throw error;
    }
    return payload;
  }
  function errorText(payload, status) {
    const code = String(payload.error || `HTTP ${status}`);
    const where = Number.isInteger(payload.question) ? ` (question ${payload.question + 1})` : "";
    const known = {
      invalid_question_options: "a choice question needs at least two different options",
      invalid_question_type: "a question has an unknown type",
      too_many_questions: "a survey can hold 50 questions",
      invalid_schedule: "the closing time must be after the opening time",
      survey_has_no_questions: "add at least one question first",
      survey_close_date_passed: "the closing time has passed; change it first",
      survey_response_limit_reached: "the response limit is reached; raise it first",
      survey_delete_forbidden: "only its creator or an owner or admin can delete this survey",
      survey_catalog_full: "this organization has 500 surveys; delete some first",
    };
    return (known[code] || code.replaceAll("_", " ")) + where;
  }
  function message(kind, value = "") {
    const dialog = [...$$("dialog[open]")].pop();
    const node = dialog?.querySelector(`[data-survey-dialog-${kind}]`) || $(`[data-survey-${kind}]`); if (!node) return;
    const slot = node.querySelector("[data-survey-message-text]") || node;
    slot.textContent = value; node.classList.toggle("hidden", !value);
  }
  function showDialog(dialog) {
    dialog.querySelectorAll("[data-survey-dialog-error], [data-survey-dialog-notice]").forEach((node) => node.classList.add("hidden"));
    if (!dialog.open) dialog.showModal();
    icons();
  }
  function selectedOrg() { return state.organizations.find((org) => org.name === state.organization); }
  function canManage() { return ["owner", "admin"].includes(selectedOrg()?.role || ""); }
  function orgPath(suffix = "") { return `/api/surveys/${encodeURIComponent(state.organization)}${suffix}`; }
  function pluginPath(suffix = "") { return `/api/plugins/${encodeURIComponent(state.organization)}/surveys${suffix}`; }
  function byId(id) { return state.surveys.find((item) => item.id === id); }
  function dateLabel(stamp) {
    return stamp ? new Date(stamp).toLocaleString(undefined, { day: "numeric", month: "short", year: "numeric", hour: "numeric", minute: "2-digit" }) : "—";
  }
  function localInput(stamp) {
    if (!stamp) return "";
    const date = new Date(stamp); date.setMinutes(date.getMinutes() - date.getTimezoneOffset());
    return date.toISOString().slice(0, 16);
  }
  function inputStamp(value) { const stamp = value ? new Date(value).getTime() : 0; return Number.isFinite(stamp) ? stamp : 0; }
  function plural(count, one, many = `${one}s`) { return `${count} ${count === 1 ? one : many}`; }
  function statusLabel(item) { const key = item.displayStatus || item.status; return `<span class="fm-label${STATUS_TONES[key] || ""}">${escapeHtml(STATUS_LABELS[key] || key)}</span>`; }
  function download(content, filename, type) {
    const url = URL.createObjectURL(new Blob([content], { type }));
    const link = document.createElement("a"); link.href = url; link.download = filename; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  function setParam(key, value) {
    const url = new URL(location.href);
    if (value) url.searchParams.set(key, value); else url.searchParams.delete(key);
    history.replaceState(history.state, "", url);
  }
  function icons() { window.lucide?.createIcons?.(); }

  function renderInstall() {
    const installed = Boolean(selectedOrg()?.installed);
    $(`[data-survey-install-panel]`)?.classList.toggle("hidden", installed);
    $(`[data-survey-app]`)?.classList.toggle("hidden", !installed);
    $$(`[data-survey-requires-install]`).forEach((node) => node.classList.toggle("hidden", !installed));
    const org = $(`[data-survey-install-org]`); if (org) org.textContent = state.organization;
    $(`[data-survey-install]`)?.classList.toggle("hidden", !canManage());
    $(`[data-survey-install-denied]`)?.classList.toggle("hidden", canManage());
  }
  function renderSummary() {
    const summary = state.summary || {};
    for (const [key, value] of [["surveys", summary.surveys], ["open", summary.open], ["drafts", summary.drafts],
      ["responses", summary.responses], ["week", summary.responsesLast7Days]]) {
      const node = $(`[data-survey-stat-${key}]`); if (node) node.textContent = String(value ?? 0);
    }
  }
  function matches(item, query, filter) {
    if (filter && (item.displayStatus || item.status) !== filter) return false;
    return !query || [item.title, item.description, item.createdBy].some((value) => String(value || "").toLowerCase().includes(query));
  }
  function renderList() {
    const query = String($(`[data-survey-search]`)?.value || "").trim().toLowerCase();
    const filter = $(`[data-survey-status-filter]`)?.value || "";
    const items = state.surveys.filter((item) => matches(item, query, filter));
    const host = $(`[data-survey-list]`); if (!host) return;
    host.innerHTML = items.map((item) => `<tr data-survey-open="${escapeHtml(item.id)}" class="cursor-pointer hover:bg-secondary">
      <td class="px-4 py-3"><p class="font-semibold text-foreground">${escapeHtml(item.title)}</p><p class="text-xs text-muted-foreground">${escapeHtml(plural(item.questionCount, "question"))} · by ${escapeHtml(item.createdBy || "—")}</p></td>
      <td class="px-4 py-3">${statusLabel(item)}</td>
      <td class="px-4 py-3 text-right font-semibold text-foreground">${escapeHtml(String(item.responseCount))}</td>
      <td class="px-4 py-3 text-xs text-muted-foreground">${item.lastResponseAt ? escapeHtml(relativeTimeLabel(item.lastResponseAt)) : "—"}</td>
      <td class="px-4 py-3 text-xs text-muted-foreground">${escapeHtml(relativeTimeLabel(item.createdAt))}</td></tr>`).join("");
    const cards = $(`[data-survey-cards]`);
    if (cards) cards.innerHTML = items.map((item) => `<li><button type="button" data-survey-open="${escapeHtml(item.id)}" class="flex w-full items-start justify-between gap-3 px-4 py-3 text-left hover:bg-secondary">
      <span class="min-w-0"><span class="block truncate font-semibold text-foreground">${escapeHtml(item.title)}</span><span class="block text-xs text-muted-foreground">${escapeHtml(plural(item.questionCount, "question"))}${item.lastResponseAt ? ` · last answer ${escapeHtml(relativeTimeLabel(item.lastResponseAt))}` : ""}</span></span>
      <span class="shrink-0 text-right"><span class="block font-semibold text-foreground">${escapeHtml(plural(item.responseCount, "response"))}</span>${statusLabel(item)}</span></button></li>`).join("");
    $(`[data-survey-empty]`)?.classList.toggle("hidden", items.length > 0);
    icons();
  }
  function render() { renderSummary(); renderList(); }

  function blankQuestion(type) {
    return { id: "", type, title: "", help: "", required: false, options: CHOICE_TYPES.includes(type) ? ["", ""] : [], scale: type === "rating" ? 5 : 0 };
  }
  function questionCard(question, index) {
    const choice = CHOICE_TYPES.includes(question.type);
    const types = Object.entries(TYPE_LABELS).map(([value, label]) => `<option value="${value}"${value === question.type ? " selected" : ""}>${label}</option>`).join("");
    const scales = [3, 4, 5, 6, 7, 8, 9, 10].map((value) => `<option value="${value}"${value === Number(question.scale) ? " selected" : ""}>1 to ${value}</option>`).join("");
    const last = state.questions.length - 1;
    return `<li data-survey-question="${index}" class="grid gap-2 rounded-md border border-border p-3">
      <div class="flex flex-wrap items-center gap-2"><span class="text-xs font-semibold text-muted-foreground">${index + 1}</span>
        <select data-field="type" aria-label="Question type" class="h-control-sm rounded-md border border-border bg-background px-2 text-control text-foreground">${types}</select>
        <label class="flex items-center gap-1 text-xs text-foreground"><input data-field="required" type="checkbox"${question.required ? " checked" : ""} />Required</label>
        <span class="ml-auto flex items-center gap-1">
          <button data-survey-question-move="-1" type="button" aria-label="Move up" class="fm-btn fm-btn-invisible fm-btn-icon fm-btn-sm"${index === 0 ? " disabled" : ""}><i data-lucide="arrow-up"></i></button>
          <button data-survey-question-move="1" type="button" aria-label="Move down" class="fm-btn fm-btn-invisible fm-btn-icon fm-btn-sm"${index === last ? " disabled" : ""}><i data-lucide="arrow-down"></i></button>
          <button data-survey-question-remove type="button" aria-label="Remove question" class="fm-btn fm-btn-invisible fm-btn-icon fm-btn-sm"><i data-lucide="trash-2"></i></button>
        </span></div>
      <input data-field="title" value="${escapeHtml(question.title)}" maxlength="300" aria-label="Question" placeholder="Question" class="h-control rounded-md border border-border bg-background px-3 text-control text-foreground" />
      <input data-field="help" value="${escapeHtml(question.help)}" maxlength="1000" aria-label="Help text" placeholder="Help text (optional)" class="h-control-sm rounded-md border border-border bg-background px-3 text-control text-foreground" />
      ${choice ? `<label class="grid gap-1 text-xs text-muted-foreground">Options, one per line<textarea data-field="options" rows="${Math.max(2, Math.min(8, question.options.length))}" class="rounded-md border border-border bg-background px-3 py-2 text-control text-foreground">${escapeHtml(question.options.join("\n"))}</textarea></label>` : ""}
      ${question.type === "rating" ? `<label class="grid gap-1 text-xs text-muted-foreground">Scale<select data-field="scale" class="h-control-sm rounded-md border border-border bg-background px-2 text-control text-foreground">${scales}</select></label>` : ""}
    </li>`;
  }
  function renderQuestions() {
    const host = $(`[data-survey-questions]`); if (!host) return;
    host.innerHTML = state.questions.map(questionCard).join("");
    $(`[data-survey-questions-empty]`)?.classList.toggle("hidden", state.questions.length > 0);
    icons();
  }
  function updateQuestion(node) {
    const card = node.closest("[data-survey-question]"); if (!card) return;
    const question = state.questions[Number(card.dataset.surveyQuestion)]; if (!question) return;
    const field = node.dataset.field;
    if (field === "required") question.required = node.checked;
    else if (field === "options") question.options = node.value.split("\n");
    else if (field === "scale") question.scale = Number(node.value) || 5;
    else if (field === "type") {
      question.type = node.value;
      if (CHOICE_TYPES.includes(question.type) && question.options.length < 2) question.options = [...question.options, "", ""].slice(0, Math.max(2, question.options.length));
      if (question.type === "rating" && !question.scale) question.scale = 5;
      renderQuestions();
    } else question[field] = node.value;
  }
  function openEditor(item = null) {
    const source = item || { title: "", description: "", thankYou: "Thank you for answering.", settings: DEFAULT_SETTINGS, questions: [] };
    const settings = { ...DEFAULT_SETTINGS, ...(source.settings || {}) };
    $(`[data-survey-editor-title]`).textContent = item ? `Edit ${item.title}` : "New survey";
    $(`[data-survey-id]`).value = item?.id || "";
    $(`[data-survey-title]`).value = source.title || "";
    $(`[data-survey-description]`).value = source.description || "";
    $(`[data-survey-thanks]`).value = source.thankYou || "";
    $(`[data-survey-opens]`).value = localInput(settings.opensAt);
    $(`[data-survey-closes]`).value = localInput(settings.closesAt);
    $(`[data-survey-max]`).value = settings.maxResponses ? String(settings.maxResponses) : "";
    $$(`[data-survey-setting]`).forEach((node) => { node.checked = Boolean(settings[node.dataset.surveySetting]); });
    state.questions = (source.questions || []).map((question) => ({ ...question, options: [...(question.options || [])] }));
    if (!item) state.questions = [blankQuestion("single")];
    renderQuestions();
    $(`[data-survey-detail]`)?.close();
    showDialog($(`[data-survey-editor]`));
  }
  function editorBody() {
    const settings = {
      opensAt: inputStamp($(`[data-survey-opens]`).value),
      closesAt: inputStamp($(`[data-survey-closes]`).value),
      maxResponses: Math.max(0, Math.floor(Number($(`[data-survey-max]`).value) || 0)),
    };
    $$(`[data-survey-setting]`).forEach((node) => { settings[node.dataset.surveySetting] = node.checked; });
    return {
      title: $(`[data-survey-title]`).value,
      description: $(`[data-survey-description]`).value,
      thankYou: $(`[data-survey-thanks]`).value,
      settings,
      questions: state.questions.map((question) => ({
        id: question.id, type: question.type, title: question.title, help: question.help, required: question.required,
        options: CHOICE_TYPES.includes(question.type) ? question.options.map((value) => value.trim()).filter(Boolean) : [],
        scale: question.type === "rating" ? Number(question.scale) || 5 : 0,
      })),
    };
  }
  async function saveEditor(event) {
    event.preventDefault();
    const id = $(`[data-survey-id]`).value;
    try {
      const payload = await request(orgPath(`/items${id ? `/${id}` : ""}`), id ? "PATCH" : "POST", editorBody());
      $(`[data-survey-editor]`).close(); message("notice", `${payload.survey.title} saved.`);
      await load(); await openDetail(payload.survey.id, payload.survey);
    } catch (error) { message("error", `Could not save the survey: ${error.message}`); }
  }

  function bar(count, total) {
    const percent = total ? Math.round((count / total) * 100) : 0;
    return `<div class="flex items-center gap-2"><div class="h-2 flex-1 overflow-hidden rounded-full bg-secondary"><div class="h-2 rounded-full bg-primary" style="width:${percent}%"></div></div><span class="w-16 shrink-0 text-right text-xs text-muted-foreground">${count} · ${percent}%</span></div>`;
  }
  function resultBlock(result, index) {
    const head = `<div class="mb-2"><p class="text-sm font-semibold text-foreground">${index + 1}. ${escapeHtml(result.title)}</p><p class="text-xs text-muted-foreground">${escapeHtml(TYPE_LABELS[result.type] || result.type)} · ${escapeHtml(plural(result.answered, "answer"))}${result.skipped ? ` · ${result.skipped} skipped` : ""}</p></div>`;
    let body = "";
    if (CHOICE_TYPES.includes(result.type)) {
      body = result.options.map((option) => `<div class="grid gap-1"><p class="text-xs text-foreground">${escapeHtml(option.label)}</p>${bar(option.count, result.answered)}</div>`).join("");
    } else if (result.type === "rating") {
      body = `<p class="text-lg font-semibold text-foreground">${result.answered ? escapeHtml(String(result.average)) : "—"} <span class="text-xs font-normal text-muted-foreground">average of ${result.distribution.length}</span></p>`
        + result.distribution.map((count, step) => `<div class="flex items-center gap-2"><span class="w-6 text-xs text-muted-foreground">${step + 1}</span><div class="flex-1">${bar(count, result.answered)}</div></div>`).join("");
    } else {
      body = result.answers.length
        ? `<ul class="divide-y divide-border rounded-md border border-border">${result.answers.map((text) => `<li class="whitespace-pre-wrap px-3 py-2 text-xs text-foreground">${escapeHtml(text)}</li>`).join("")}</ul>${result.answered > result.answers.length ? `<p class="mt-1 text-xs text-muted-foreground">Showing the ${result.answers.length} newest. Export CSV for all of them.</p>` : ""}`
        : `<p class="text-xs text-muted-foreground">No answers yet.</p>`;
    }
    return `<section class="grid gap-2 rounded-md border border-border p-3">${head}${body}</section>`;
  }
  function answerText(question, value) {
    if (Array.isArray(value)) return value.join(", ");
    if (question?.type === "rating" && value) return `${value} of ${question.scale}`;
    return String(value ?? "");
  }
  function responsesBlock(survey) {
    if (state.responses === null) return `<p class="text-xs text-muted-foreground">Loading responses…</p>`;
    if (!state.responses.length) return `<p class="text-xs text-muted-foreground">No responses yet. Share the link to collect some.</p>`;
    const questions = Object.fromEntries((survey.questions || []).map((question) => [question.id, question]));
    return `<ul class="grid gap-2">${state.responses.map((response) => `<li class="rounded-md border border-border p-3">
      <div class="mb-2 flex items-center justify-between gap-2"><p class="text-xs text-muted-foreground">${escapeHtml(dateLabel(response.submittedAt))}</p><button type="button" data-survey-response-delete="${escapeHtml(response.id)}" aria-label="Delete response" class="fm-btn fm-btn-invisible fm-btn-icon fm-btn-sm"><i data-lucide="trash-2"></i></button></div>
      <dl class="grid gap-1">${(survey.questions || []).filter((question) => response.answers[question.id] !== undefined).map((question) => `<div><dt class="text-xs text-muted-foreground">${escapeHtml(question.title)}</dt><dd class="whitespace-pre-wrap text-sm text-foreground">${escapeHtml(answerText(questions[question.id], response.answers[question.id]))}</dd></div>`).join("") || `<p class="text-xs text-muted-foreground">Answers to removed questions only.</p>`}</dl></li>`).join("")}</ul>`;
  }
  function activityBlock(survey) {
    const activity = survey.activity;
    if (!activity) return `<p class="text-xs text-muted-foreground">Loading activity…</p>`;
    const recent = activity.recent || [];
    if (!activity.views && !activity.interactions && !recent.length) {
      return `<p class="text-xs text-muted-foreground">No one has opened the survey link yet. Opening it with Preview does not count.</p>`;
    }
    const facts = [plural(activity.views, "view"), plural(activity.visitors, "visitor"), plural(activity.interactions, "interaction"),
      `${activity.starts || 0} started`, `${activity.submits || 0} sent`];
    if (activity.lastActivityAt) facts.push(`last activity ${relativeTimeLabel(activity.lastActivityAt)}`);
    return `<p class="mb-3 text-sm text-foreground"><span class="font-semibold">Last ${escapeHtml(String(activity.days || 30))} days:</span> ${escapeHtml(facts.join(" · "))}</p>
      <ul class="divide-y divide-border border-y border-border">${recent.map((item) => `<li class="flex flex-wrap items-center justify-between gap-x-3 gap-y-1 py-2"><span class="min-w-0 text-sm text-foreground">${escapeHtml(item.label || item.kind)}</span><span class="text-xs text-muted-foreground">Visitor ${escapeHtml(item.visitor || "unknown")} · ${escapeHtml(relativeTimeLabel(item.at))}</span></li>`).join("")}</ul>
      <p class="mt-2 text-xs text-muted-foreground">Visitors are counted by a one-way hash of their connection, never their address. ForkMesh keeps the newest 1,000 events per survey for 90 days.</p>`;
  }
  function renderTabs() {
    $$(`[data-survey-tab]`).forEach((tab) => {
      const active = tab.dataset.surveyTab === state.tab;
      tab.setAttribute("aria-selected", String(active));
    });
  }
  function renderDetail(survey) {
    const head = $(`[data-survey-detail-head]`); const body = $(`[data-survey-detail-body]`);
    const actions = $(`[data-survey-detail-actions]`); const link = $(`[data-survey-detail-link]`);
    if (!head || !body || !actions || !link) return;
    const settings = { ...DEFAULT_SETTINGS, ...(survey.settings || {}) };
    const facts = [plural(survey.responseCount, "response"), plural(survey.questionCount, "question")];
    if (settings.opensAt) facts.push(`opens ${dateLabel(settings.opensAt)}`);
    if (settings.closesAt) facts.push(`closes ${dateLabel(settings.closesAt)}`);
    if (settings.maxResponses) facts.push(`stops at ${settings.maxResponses}`);
    if (settings.oneResponsePerPerson) facts.push("one per person");
    head.innerHTML = `<h2 id="survey-detail-title" class="truncate text-sm font-semibold">${escapeHtml(survey.title)} ${statusLabel(survey)}</h2><p class="text-xs font-normal text-muted-foreground">${escapeHtml(facts.join(" · "))}</p>`;
    link.innerHTML = survey.publicUrl
      ? `<div class="mb-3 flex flex-wrap items-center gap-2"><input readonly value="${escapeHtml(survey.publicUrl)}" aria-label="Survey link" class="h-control-sm min-w-0 flex-1 rounded-md border border-border bg-background px-2 text-xs text-muted-foreground" /><button type="button" data-survey-copy-link class="fm-btn fm-btn-sm">Copy link</button><a href="${escapeHtml(`${survey.publicUrl}&preview=1`)}" target="_blank" rel="noopener noreferrer" class="fm-btn fm-btn-sm">Preview</a></div>`
      : `<p class="mb-3 text-xs text-muted-foreground">Open the survey to get a link anyone can answer.</p>`;
    renderTabs();
    if (state.tab === "results") {
      body.innerHTML = survey.results
        ? (survey.results.length ? `<div class="grid gap-3">${survey.results.map(resultBlock).join("")}</div>${survey.resultsCounted < survey.responseCount ? `<p class="mt-2 text-xs text-muted-foreground">Results count the ${survey.resultsCounted} newest responses.</p>` : ""}` : `<p class="text-xs text-muted-foreground">This survey has no questions yet.</p>`)
        : `<p class="text-xs text-muted-foreground">Loading results…</p>`;
    } else if (state.tab === "activity") body.innerHTML = activityBlock(survey);
    else body.innerHTML = responsesBlock(survey);
    const status = survey.status;
    const buttons = [];
    if (survey.canDelete) buttons.push(`<button type="button" data-survey-action="delete" class="fm-btn fm-btn-sm fm-btn-danger mr-auto">Delete</button>`);
    buttons.push(`<button type="button" data-survey-action="duplicate" class="fm-btn fm-btn-sm">Duplicate</button>`);
    buttons.push(`<button type="button" data-survey-action="edit" class="fm-btn fm-btn-sm">Edit</button>`);
    if (survey.responseCount) buttons.push(`<button type="button" data-survey-action="export" class="fm-btn fm-btn-sm"><i data-lucide="download"></i>Export CSV</button>`);
    if (status === "open") buttons.push(`<button type="button" data-survey-action="close" class="fm-btn fm-btn-sm">Close survey</button>`);
    else buttons.push(`<button type="button" data-survey-action="open" class="fm-btn fm-btn-sm"><i data-lucide="send"></i>${status === "draft" ? "Open survey" : "Reopen"}</button>`);
    actions.innerHTML = buttons.join("");
    icons();
  }
  async function loadResponses() {
    if (!state.detailId) return;
    const id = state.detailId;
    try {
      const payload = await request(orgPath(`/items/${id}/responses`));
      if (state.detailId !== id) return;
      state.responses = payload.responses || [];
    } catch (error) { state.responses = []; message("error", `Could not load responses: ${error.message}`); }
    if (state.detail) renderDetail(state.detail);
  }
  async function openDetail(id, fresh = null) {
    if (state.detailId !== id) { state.tab = "results"; state.responses = null; }
    state.detailId = id; setParam("survey", id);
    const dialog = $(`[data-survey-detail]`);
    state.detail = fresh || { ...(byId(id) || {}), results: null };
    renderDetail(state.detail); showDialog(dialog);
    if (fresh) return;
    try {
      const payload = await request(orgPath(`/items/${id}`));
      if (state.detailId !== id) return;
      state.detail = payload.survey; renderDetail(state.detail);
    } catch (error) { message("error", `Could not load the survey: ${error.message}`); }
  }
  async function detailAction(action) {
    const survey = state.detail; if (!survey) return;
    if (action === "edit") { if (!survey.questions) return; return openEditor(survey); }
    if (action === "delete") {
      if (!confirm(`Delete ${survey.title} and its ${plural(survey.responseCount, "response")}? This cannot be undone.`)) return;
      try { await request(orgPath(`/items/${survey.id}`), "DELETE", {}); $(`[data-survey-detail]`).close();
        message("notice", `${survey.title} deleted.`); await load(); }
      catch (error) { message("error", `Could not delete the survey: ${error.message}`); }
      return;
    }
    if (action === "export") {
      try { const payload = await request(orgPath(`/items/${survey.id}/export`)); download(payload.csv, payload.filename, "text/csv"); }
      catch (error) { message("error", `Export failed: ${error.message}`); }
      return;
    }
    if (action === "close" && !confirm(`Close ${survey.title}? Its link stops taking answers until you reopen it.`)) return;
    try {
      const payload = await request(orgPath(`/items/${survey.id}/${action}`), "POST", {});
      state.responses = null;
      await load(); await openDetail(payload.survey.id, payload.survey);
      if (state.tab === "responses") await loadResponses();
      message("notice", { open: "The survey is open. Share its link.", close: "The survey is closed.", duplicate: "A draft copy was made." }[action] || "Saved.");
    } catch (error) { message("error", `Could not ${action} the survey: ${error.message}`); }
  }
  async function deleteResponse(id) {
    if (!confirm("Delete this response? This cannot be undone.")) return;
    try {
      const payload = await request(orgPath(`/items/${state.detailId}/responses/${id}`), "DELETE", {});
      state.detail = payload.survey; state.responses = (state.responses || []).filter((item) => item.id !== id);
      renderDetail(state.detail); message("notice", "Response deleted."); await load();
    } catch (error) { message("error", `Could not delete the response: ${error.message}`); }
  }

  async function install() {
    try { await request(pluginPath("/install"), "POST", {}); selectedOrg().installed = true;
      message("notice", "Surveys installed."); await load(); }
    catch (error) { message("error", `Could not install: ${error.message}`); }
  }
  async function exportAll() {
    try { const payload = await request(pluginPath("/export"));
      download(JSON.stringify(payload.export || {}, null, 2), payload.filename || "forkmesh-surveys.json", "application/json"); }
    catch (error) { message("error", `Export failed: ${error.message}`); }
  }
  async function uninstall() {
    if (!confirm(`Uninstall Surveys from ${state.organization}? An export downloads first, then every survey and response is removed and shared links stop working.`)) return;
    try { const snapshot = await request(pluginPath("/export"));
      if (snapshot.hasData) download(JSON.stringify(snapshot.export || {}, null, 2), snapshot.filename, "application/json");
      await request(pluginPath("/uninstall"), "POST", {}); selectedOrg().installed = false;
      $(`[data-survey-manage-dialog]`).close(); message("notice", "Surveys uninstalled."); await load(); }
    catch (error) { message("error", `Could not uninstall: ${error.message}`); }
  }
  async function load() {
    renderInstall(); if (!selectedOrg()?.installed) return;
    try {
      const payload = await request(orgPath());
      state.surveys = payload.surveys || []; state.summary = payload.summary || {};
      if (![...$$("dialog[open]")].length) message("error", "");
      render();
    } catch (error) {
      if (error.status === 409) { selectedOrg().installed = false; renderInstall(); }
      else message("error", `Surveys is unavailable: ${error.message}`);
    }
  }
  async function init() {
    if (!session()?.sessionToken) return message("error", "Sign in to use Surveys.");
    try {
      const payload = await request("/api/surveys"); state.organizations = payload.organizations || [];
      if (!state.organizations.length) return message("error", "Join or create an organization before using Surveys.");
      const select = $(`[data-survey-organization]`);
      select.innerHTML = state.organizations.map((org) => `<option value="${escapeHtml(org.name)}">${escapeHtml(org.name)} · ${escapeHtml(org.role)}${org.installed ? "" : " · not installed"}</option>`).join("");
      const wanted = state.organizations.find((org) => org.name === requested.org)
        || state.organizations.find((org) => org.installed) || state.organizations[0];
      state.organization = wanted.name; select.value = state.organization;
      select.addEventListener("change", () => { state.organization = select.value; setParam("org", select.value); setParam("survey", ""); void load(); });
      await load();
      if (requested.survey && byId(requested.survey)) {
        await openDetail(requested.survey);
        if (requested.tab && state.detail) { state.tab = requested.tab; renderDetail(state.detail); if (state.tab === "responses") void loadResponses(); }
      }
    } catch (error) { return message("error", `Surveys is unavailable: ${error.message}`); }
    $(`[data-survey-install]`)?.addEventListener("click", () => void install());
    $(`[data-survey-new]`)?.addEventListener("click", () => openEditor());
    $(`[data-survey-manage]`)?.addEventListener("click", () => showDialog($(`[data-survey-manage-dialog]`)));
    $(`[data-survey-export]`)?.addEventListener("click", () => void exportAll());
    $(`[data-survey-uninstall]`)?.addEventListener("click", () => void uninstall());
    $(`[data-survey-search]`)?.addEventListener("input", renderList);
    $(`[data-survey-status-filter]`)?.addEventListener("change", renderList);
    for (const selector of [`[data-survey-list]`, `[data-survey-cards]`]) {
      $(selector)?.addEventListener("click", (event) => { const row = event.target.closest("[data-survey-open]"); if (row) void openDetail(row.dataset.surveyOpen); });
    }
    $(`[data-survey-form]`)?.addEventListener("submit", saveEditor);
    $(`[data-survey-question-add]`)?.addEventListener("click", () => {
      if (state.questions.length >= 50) return message("error", "A survey can hold 50 questions.");
      state.questions.push(blankQuestion($(`[data-survey-question-type]`).value)); renderQuestions();
      $(`[data-survey-questions] [data-survey-question="${state.questions.length - 1}"] [data-field="title"]`)?.focus();
    });
    $(`[data-survey-questions]`)?.addEventListener("input", (event) => { if (event.target.dataset.field && event.target.dataset.field !== "type") updateQuestion(event.target); });
    $(`[data-survey-questions]`)?.addEventListener("change", (event) => { if (event.target.dataset.field) updateQuestion(event.target); });
    $(`[data-survey-questions]`)?.addEventListener("click", (event) => {
      const card = event.target.closest("[data-survey-question]"); if (!card) return;
      const index = Number(card.dataset.surveyQuestion);
      const move = event.target.closest("[data-survey-question-move]");
      if (move) {
        const target = index + Number(move.dataset.surveyQuestionMove);
        if (target < 0 || target >= state.questions.length) return;
        [state.questions[index], state.questions[target]] = [state.questions[target], state.questions[index]];
        renderQuestions(); return;
      }
      if (event.target.closest("[data-survey-question-remove]")) { state.questions.splice(index, 1); renderQuestions(); }
    });
    $$(`[data-survey-tab]`).forEach((tab) => tab.addEventListener("click", () => {
      state.tab = tab.dataset.surveyTab; if (state.detail) renderDetail(state.detail);
      if (state.tab === "responses" && state.responses === null) void loadResponses();
    }));
    $(`[data-survey-detail-actions]`)?.addEventListener("click", (event) => { const button = event.target.closest("[data-survey-action]"); if (button) void detailAction(button.dataset.surveyAction); });
    $(`[data-survey-detail]`)?.addEventListener("click", async (event) => {
      const remove = event.target.closest("[data-survey-response-delete]");
      if (remove) return void deleteResponse(remove.dataset.surveyResponseDelete);
      if (!event.target.closest("[data-survey-copy-link]")) return;
      const url = state.detail?.publicUrl; if (!url) return;
      try { await navigator.clipboard.writeText(url); message("notice", "Survey link copied."); } catch { message("error", "The link could not be copied."); }
    });
    $(`[data-survey-detail]`)?.addEventListener("close", () => { setParam("survey", ""); state.detailId = ""; state.detail = null; });
    $$(`[data-survey-dialog-close]`).forEach((button) => button.addEventListener("click", () => button.closest("dialog")?.close()));
  }
  return init();
});

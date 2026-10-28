window.ForkMeshDashboard.registerPage("invoices", ({
  select: $, selectAll: $$, session, escapeHtml, relativeTimeLabel,
}) => {
  const DAY_MS = 24 * 60 * 60 * 1000;
  const STATUS_LABELS = { draft: "Draft", sent: "Sent", partial: "Part paid", overdue: "Overdue", paid: "Paid", void: "Void" };
  const STATUS_TONES = { draft: "", sent: " fm-label-accent", partial: " fm-label-attention", overdue: " fm-label-danger", paid: " fm-label-success", void: "" };
  const EVENT_LABELS = {
    created: "Created", sent: "Emailed", reminder: "Reminder emailed", failed: "Email failed",
    opened: "Email opened", viewed: "Invoice viewed", clicked: "Pay link clicked", paid: "Payment recorded",
    reopened: "Reopened", voided: "Voided", overdue: "Became overdue",
  };
  const EVENT_ICONS = {
    created: "file-plus", sent: "send", reminder: "bell", failed: "circle-alert", opened: "mail-open",
    viewed: "eye", clicked: "mouse-pointer-click", paid: "badge-check", reopened: "rotate-ccw", voided: "ban", overdue: "clock",
  };
  const state = {
    organizations: [], organization: "", invoices: [], summary: {}, activity: [], settings: {},
    crmInstalled: false, contacts: null, detailId: "", lines: [],
  };
  const params = new URL(location.href).searchParams;
  const requested = { org: params.get("org") || "", invoice: params.get("invoice") || "", contact: params.get("contact") || "" };

  async function request(path, method = "GET", body = null) {
    const headers = { accept: "application/json" };
    const token = session()?.sessionToken;
    if (token) headers.authorization = `Bearer ${token}`;
    if (body !== null) headers["content-type"] = "application/json";
    const response = await fetch(path, { method, headers, cache: "no-store",
      body: body === null ? undefined : JSON.stringify(body) });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(String(payload.error || `HTTP ${response.status}`).replaceAll("_", " "));
      error.status = response.status; throw error;
    }
    return payload;
  }
  function message(kind, value = "") {
    const node = $(`[data-invoice-${kind}]`); if (!node) return;
    const slot = node.querySelector("[data-invoice-message-text]") || node;
    slot.textContent = value; node.classList.toggle("hidden", !value);
  }
  function selectedOrg() { return state.organizations.find((org) => org.name === state.organization); }
  function canManage() { return ["owner", "admin"].includes(selectedOrg()?.role || ""); }
  function orgPath(suffix = "") { return `/api/invoices/${encodeURIComponent(state.organization)}${suffix}`; }
  function pluginPath(suffix = "") { return `/api/plugins/${encodeURIComponent(state.organization)}/invoices${suffix}`; }
  function byId(id) { return state.invoices.find((item) => item.id === id); }
  function money(amount, currency) {
    const value = Number(amount || 0) / 100;
    try { return new Intl.NumberFormat(undefined, { style: "currency", currency: currency || "USD" }).format(value); }
    catch { return `${currency || ""} ${value.toFixed(2)}`.trim(); }
  }
  function moneyMap(map) {
    const entries = Object.entries(map || {}).filter(([, value]) => value);
    if (!entries.length) return money(0, state.settings.currency || "USD");
    return entries.map(([currency, value]) => money(value, currency)).join(" · ");
  }
  function dateLabel(stamp) {
    return stamp ? new Date(stamp).toLocaleDateString(undefined, { timeZone: "UTC", day: "numeric", month: "short", year: "numeric" }) : "—";
  }
  function dateInput(stamp) { return stamp ? new Date(stamp).toISOString().slice(0, 10) : ""; }
  function inputStamp(value) { const stamp = Date.parse(`${value}T00:00:00Z`); return Number.isFinite(stamp) ? stamp : 0; }
  function cents(value) { const number = Number.parseFloat(String(value || "").replace(/[^0-9.\-]/g, "")); return Number.isFinite(number) ? Math.max(0, Math.round(number * 100)) : 0; }
  function clientLabel(item) { return item.client?.name || item.client?.company || item.client?.email || "No client"; }
  function statusLabel(item) { const key = item.displayStatus || item.status; return `<span class="fm-label${STATUS_TONES[key] || ""}">${escapeHtml(STATUS_LABELS[key] || key)}</span>`; }
  function crmLink(contactId, label) {
    return `<a href="/dashboard/crm?org=${encodeURIComponent(state.organization)}&contact=${encodeURIComponent(contactId)}" class="text-primary hover:underline">${escapeHtml(label)}</a>`;
  }
  function downloadJson(payload, filename) {
    const url = URL.createObjectURL(new Blob([JSON.stringify(payload, null, 2)], { type: "application/json" }));
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
    $(`[data-invoice-install-panel]`)?.classList.toggle("hidden", installed);
    $(`[data-invoice-app]`)?.classList.toggle("hidden", !installed);
    $$(`[data-invoice-requires-install]`).forEach((node) => node.classList.toggle("hidden", !installed));
    const org = $(`[data-invoice-install-org]`); if (org) org.textContent = state.organization;
    $(`[data-invoice-install]`)?.classList.toggle("hidden", !canManage());
    $(`[data-invoice-install-denied]`)?.classList.toggle("hidden", canManage());
  }
  function stat(key, value, detail = "") {
    const node = $(`[data-invoice-stat-${key}]`); if (node) node.textContent = value;
    const extra = $(`[data-invoice-stat-${key}-count]`) || $(`[data-invoice-stat-${key}-detail]`); if (extra) extra.textContent = detail;
  }
  function plural(count, one, many = `${one}s`) { return `${count} ${count === 1 ? one : many}`; }
  function renderSummary() {
    const summary = state.summary || {}; const counts = summary.counts || {};
    stat("outstanding", moneyMap(summary.outstanding), plural(counts.sent || 0, "unpaid invoice"));
    stat("overdue", moneyMap(summary.overdueAmount), plural(counts.overdue || 0, "overdue invoice"));
    stat("paid", moneyMap(summary.paidLast30Days), plural(counts.paid || 0, "paid invoice") + " in all");
    stat("opened", summary.sent ? `${summary.openRate || 0}%` : "—",
      summary.sent ? `${summary.opened || 0} of ${summary.sent} opened · ${summary.viewed || 0} viewed · ${summary.clicked || 0} clicked pay` : "No invoices sent yet");
    stat("days", summary.averageDaysToPay ? `${summary.averageDaysToPay} days` : "—",
      summary.averageHoursToOpen ? `Opened ${summary.averageHoursToOpen} h after sending, on average` : "From sending to paid");
  }
  function engagement(item) {
    if (!item.sentAt) return `<span class="text-xs text-muted-foreground">Not sent</span>`;
    const parts = [
      `<span title="Email opens"><i data-lucide="mail-open" class="inline h-3 w-3"></i> ${item.opens}</span>`,
      `<span title="Invoice page views"><i data-lucide="eye" class="inline h-3 w-3"></i> ${item.views}</span>`,
      `<span title="Pay link clicks"><i data-lucide="mouse-pointer-click" class="inline h-3 w-3"></i> ${item.clicks}</span>`,
    ].join(" ");
    const last = item.lastActivityAt ? `<p class="text-xs text-muted-foreground">${escapeHtml(relativeTimeLabel(item.lastActivityAt))}</p>` : "";
    return `<div class="flex gap-3 text-xs text-muted-foreground">${parts}</div>${last}`;
  }
  function matches(item, query, filter) {
    const key = item.displayStatus || item.status;
    if (filter === "open" && !["sent", "partial", "overdue"].includes(key)) return false;
    if (filter && filter !== "open" && key !== filter && !(filter === "paid" && item.status === "paid")) return false;
    return !query || [item.number, item.client?.name, item.client?.company, item.client?.email]
      .some((value) => String(value || "").toLowerCase().includes(query));
  }
  function renderList() {
    const query = String($(`[data-invoice-search]`)?.value || "").trim().toLowerCase();
    const filter = $(`[data-invoice-status-filter]`)?.value || "";
    const items = state.invoices.filter((item) => matches(item, query, filter));
    const host = $(`[data-invoice-list]`); if (!host) return;
    host.innerHTML = items.map((item) => `<tr data-invoice-open="${escapeHtml(item.id)}" class="cursor-pointer hover:bg-secondary">
      <td class="px-4 py-3"><p class="font-semibold text-foreground">${escapeHtml(item.number)}</p><p class="text-xs text-muted-foreground">${escapeHtml(clientLabel(item))}${item.contactId ? " · CRM" : ""}</p></td>
      <td class="px-4 py-3">${statusLabel(item)}</td>
      <td class="px-4 py-3 text-right"><p class="font-semibold text-foreground">${escapeHtml(money(item.total, item.currency))}</p>${item.balance && item.balance !== item.total ? `<p class="text-xs text-muted-foreground">${escapeHtml(money(item.balance, item.currency))} due</p>` : ""}</td>
      <td class="px-4 py-3 text-xs text-muted-foreground">${escapeHtml(dateLabel(item.dueAt))}</td>
      <td class="px-4 py-3">${engagement(item)}</td></tr>`).join("");
    $(`[data-invoice-empty]`)?.classList.toggle("hidden", items.length > 0);
    icons();
  }
  function renderActivity() {
    const host = $(`[data-invoice-activity]`); if (!host) return;
    host.innerHTML = state.activity.map((event) => `<li><button type="button" data-invoice-open="${escapeHtml(event.invoiceId)}" class="flex w-full items-start gap-2 px-4 py-2 text-left hover:bg-secondary">
      <i data-lucide="${EVENT_ICONS[event.kind] || "circle"}" class="mt-0.5 h-4 w-4 shrink-0 text-muted-foreground"></i>
      <span class="min-w-0"><span class="block text-xs text-foreground">${escapeHtml(EVENT_LABELS[event.kind] || event.kind)} · ${escapeHtml(event.number)}</span>
      <span class="block truncate text-xs text-muted-foreground">${escapeHtml([event.clientName, event.client, event.kind === "paid" && event.amount ? money(event.amount, event.currency) : "", relativeTimeLabel(event.at)].filter(Boolean).join(" · "))}</span></span></button></li>`).join("");
    $(`[data-invoice-activity-empty]`)?.classList.toggle("hidden", state.activity.length > 0);
    icons();
  }
  function render() { renderSummary(); renderList(); renderActivity(); }

  async function loadContacts() {
    if (!state.crmInstalled) { state.contacts = []; return; }
    if (state.contacts) return;
    try { state.contacts = (await request(orgPath("/contacts"))).contacts || []; }
    catch { state.contacts = []; }
  }
  function contactOptions(selected) {
    const node = $(`[data-invoice-contact]`); if (!node) return;
    node.innerHTML = `<option value="">Not linked to a CRM contact</option>` + (state.contacts || []).map((contact) => `<option value="${escapeHtml(contact.id)}">${escapeHtml([contact.name || contact.email, contact.company].filter(Boolean).join(" · "))}</option>`).join("");
    node.value = selected || "";
    const field = $(`[data-invoice-contact-field]`);
    field?.classList.toggle("hidden", !state.crmInstalled); field?.classList.toggle("grid", state.crmInstalled);
  }
  function lineAmount(line) { return Math.round((Number(line.quantity) || 0) * cents(line.unit)); }
  function renderLines() {
    const host = $(`[data-invoice-lines]`); if (!host) return;
    const currency = ($(`[data-invoice-currency]`)?.value || state.settings.currency || "USD").toUpperCase();
    host.innerHTML = state.lines.map((line, index) => `<tr>
      <td class="px-2 py-1"><input data-invoice-line="${index}" data-field="description" value="${escapeHtml(line.description)}" maxlength="1000" aria-label="Description" class="h-control-sm w-full rounded-md border border-border bg-background px-2 text-control text-foreground" /></td>
      <td class="px-2 py-1"><input data-invoice-line="${index}" data-field="quantity" value="${escapeHtml(String(line.quantity))}" type="number" min="0" step="any" aria-label="Quantity" class="h-control-sm w-full rounded-md border border-border bg-background px-2 text-control text-foreground" /></td>
      <td class="px-2 py-1"><input data-invoice-line="${index}" data-field="unit" value="${escapeHtml(line.unit)}" inputmode="decimal" aria-label="Unit price" placeholder="0.00" class="h-control-sm w-full rounded-md border border-border bg-background px-2 text-control text-foreground" /></td>
      <td data-invoice-line-amount="${index}" class="px-2 py-1 text-right text-foreground">${escapeHtml(money(lineAmount(line), currency))}</td>
      <td class="px-1 py-1"><button data-invoice-line-remove="${index}" type="button" aria-label="Remove line" class="fm-btn fm-btn-invisible fm-btn-icon fm-btn-sm"><i data-lucide="trash-2"></i></button></td></tr>`).join("");
    renderTotals(); icons();
  }
  function renderTotals() {
    const currency = ($(`[data-invoice-currency]`)?.value || state.settings.currency || "USD").toUpperCase();
    const subtotal = state.lines.reduce((sum, line) => sum + lineAmount(line), 0);
    const tax = Math.round(subtotal * Math.round((Number($(`[data-invoice-tax]`)?.value) || 0) * 100) / 10000);
    state.lines.forEach((line, index) => { const cell = $(`[data-invoice-line-amount="${index}"]`); if (cell) cell.textContent = money(lineAmount(line), currency); });
    $(`[data-invoice-subtotal]`).textContent = money(subtotal, currency);
    $(`[data-invoice-taxtotal]`).textContent = money(tax, currency);
    $(`[data-invoice-total]`).textContent = money(subtotal + tax, currency);
  }
  function fillClientFromContact(contactId) {
    const contact = (state.contacts || []).find((item) => item.id === contactId); if (!contact) return;
    for (const key of ["name", "email", "company"]) {
      const node = $(`[data-invoice-client-${key}]`); if (node && contact[key]) node.value = contact[key];
    }
  }
  async function openEditor(item = null, seed = {}) {
    await loadContacts();
    const settings = state.settings || {};
    const source = item || seed;
    $(`[data-invoice-editor-title]`).textContent = item ? `Edit ${item.number}` : "New invoice";
    $(`[data-invoice-id]`).value = item?.id || "";
    contactOptions(source.contactId || "");
    for (const key of ["name", "email", "company", "address"]) $(`[data-invoice-client-${key}]`).value = source.client?.[key] || "";
    if (!item && source.contactId) fillClientFromContact(source.contactId);
    $(`[data-invoice-number]`).value = item?.number || "";
    const issued = source.issuedAt || Date.parse(new Date().toISOString().slice(0, 10) + "T00:00:00Z");
    $(`[data-invoice-issued]`).value = dateInput(issued);
    $(`[data-invoice-due]`).value = dateInput(item?.dueAt || issued + Number(settings.dueDays ?? 30) * DAY_MS);
    $(`[data-invoice-currency]`).value = source.currency || settings.currency || "USD";
    $(`[data-invoice-tax]`).value = String(((source.taxRate ?? settings.taxRate) || 0) / 100);
    $(`[data-invoice-payment-url]`).value = source.paymentUrl ?? settings.paymentUrl ?? "";
    $(`[data-invoice-notes]`).value = source.notes || "";
    $(`[data-invoice-terms]`).value = source.terms || "";
    state.lines = (source.items || []).map((line) => ({ description: line.description, quantity: line.quantity, unit: (line.unitAmount / 100).toFixed(2) }));
    if (!state.lines.length) state.lines = [{ description: "", quantity: 1, unit: "" }];
    renderLines();
    $(`[data-invoice-delete]`).classList.toggle("hidden", !item || !["draft", "void"].includes(item.status));
    $(`[data-invoice-detail]`)?.close();
    $(`[data-invoice-editor]`).showModal();
  }
  function editorBody() {
    const body = {
      client: Object.fromEntries(["name", "email", "company", "address"].map((key) => [key, $(`[data-invoice-client-${key}]`).value])),
      currency: $(`[data-invoice-currency]`).value.toUpperCase(),
      taxRate: Math.round((Number($(`[data-invoice-tax]`).value) || 0) * 100),
      issuedAt: inputStamp($(`[data-invoice-issued]`).value),
      dueAt: inputStamp($(`[data-invoice-due]`).value),
      paymentUrl: $(`[data-invoice-payment-url]`).value.trim(),
      notes: $(`[data-invoice-notes]`).value,
      terms: $(`[data-invoice-terms]`).value,
      items: state.lines.map((line) => ({ description: line.description, quantity: Number(line.quantity) || 0, unitAmount: cents(line.unit) })),
    };
    const number = $(`[data-invoice-number]`).value.trim(); if (number) body.number = number;
    if (state.crmInstalled) body.contactId = $(`[data-invoice-contact]`).value;
    return body;
  }
  async function saveEditor(event) {
    event.preventDefault();
    const id = $(`[data-invoice-id]`).value;
    try {
      const payload = await request(orgPath(`/items${id ? `/${id}` : ""}`), id ? "PATCH" : "POST", editorBody());
      $(`[data-invoice-editor]`).close(); message("notice", `Invoice ${payload.invoice.number} saved.`);
      await load(); openDetail(payload.invoice.id);
    } catch (error) { message("error", `Could not save the invoice: ${error.message}`); }
  }
  async function removeInvoice(id) {
    const item = byId(id); if (!item || !confirm(`Delete invoice ${item.number}? This cannot be undone.`)) return;
    try { await request(orgPath(`/items/${id}`), "DELETE", {}); $$("dialog[open]").forEach((dialog) => dialog.close());
      message("notice", `Invoice ${item.number} deleted.`); await load(); }
    catch (error) { message("error", `Could not delete the invoice: ${error.message}`); }
  }

  function timeline(events) {
    if (!events?.length) return `<p class="text-xs text-muted-foreground">Nothing has happened to this invoice yet.</p>`;
    return `<ul class="divide-y divide-border rounded-md border border-border">${events.map((event) => {
      const detail = [event.to ? `to ${event.to}` : "", event.client ? `in ${event.client}` : "",
        event.amount ? money(event.amount, byId(state.detailId)?.currency) : "", event.method, event.actor ? `by ${event.actor}` : "", event.error]
        .filter(Boolean).join(" · ");
      return `<li class="flex items-start gap-2 px-3 py-2"><i data-lucide="${EVENT_ICONS[event.kind] || "circle"}" class="mt-0.5 h-4 w-4 shrink-0 text-muted-foreground"></i><div class="min-w-0"><p class="text-xs text-foreground">${escapeHtml(EVENT_LABELS[event.kind] || event.kind)} <span class="text-muted-foreground">· ${escapeHtml(relativeTimeLabel(event.at))}</span></p>${detail ? `<p class="truncate text-xs text-muted-foreground">${escapeHtml(detail)}</p>` : ""}</div></li>`;
    }).join("")}</ul>`;
  }
  function metric(label, value, hint = "") {
    return `<div class="p-3"><p class="text-xs text-muted-foreground">${escapeHtml(label)}</p><p class="text-base font-semibold text-foreground">${escapeHtml(String(value))}</p>${hint ? `<p class="text-xs text-muted-foreground">${escapeHtml(hint)}</p>` : ""}</div>`;
  }
  function renderDetail(invoice) {
    const head = $(`[data-invoice-detail-head]`); const body = $(`[data-invoice-detail-body]`); const actions = $(`[data-invoice-detail-actions]`);
    if (!head || !body || !actions) return;
    const client = invoice.contactId && invoice.contact ? crmLink(invoice.contactId, clientLabel(invoice)) : escapeHtml(clientLabel(invoice));
    head.innerHTML = `<h2 class="truncate text-sm font-semibold">${escapeHtml(invoice.number)} ${statusLabel(invoice)}</h2><p class="text-xs font-normal text-muted-foreground">${client}${invoice.client?.company && invoice.client?.name ? ` · ${escapeHtml(invoice.client.company)}` : ""}${invoice.contactId && !invoice.contact ? " · CRM contact removed" : ""}</p>`;
    const lines = invoice.items.map((line) => `<tr><td class="py-1 pr-2">${escapeHtml(line.description || "Item")}</td><td class="py-1 pr-2 text-right text-muted-foreground">${escapeHtml(String(line.quantity))} × ${escapeHtml(money(line.unitAmount, invoice.currency))}</td><td class="py-1 text-right">${escapeHtml(money(Math.round(line.quantity * line.unitAmount), invoice.currency))}</td></tr>`).join("");
    const openHint = invoice.firstOpenedAt ? `first ${relativeTimeLabel(invoice.firstOpenedAt)}` : invoice.sentAt ? "not yet" : "";
    const viewHint = invoice.firstViewedAt ? `first ${relativeTimeLabel(invoice.firstViewedAt)}` : invoice.sentAt ? "not yet" : "";
    body.innerHTML = `<div class="grid gap-4">
      <div class="grid grid-cols-2 divide-x divide-y divide-border overflow-hidden rounded-md border border-border sm:grid-cols-4 sm:divide-y-0">
        ${metric("Total", money(invoice.total, invoice.currency), invoice.paidAmount ? `${money(invoice.paidAmount, invoice.currency)} paid` : "")}
        ${metric("Due", dateLabel(invoice.dueAt), invoice.balance ? `${money(invoice.balance, invoice.currency)} to pay` : invoice.status === "paid" ? `paid ${dateLabel(invoice.paidAt)}` : "")}
        ${metric("Email opens", invoice.sentAt ? invoice.opens : "—", openHint)}
        ${metric("Page views", invoice.sentAt ? invoice.views : "—", invoice.clicks ? `${viewHint} · ${invoice.clicks} pay ${invoice.clicks === 1 ? "click" : "clicks"}` : viewHint)}
      </div>
      ${invoice.sentAt ? `<p class="text-xs text-muted-foreground">Emailed to ${escapeHtml(invoice.sentTo || invoice.client?.email || "the client")} ${escapeHtml(relativeTimeLabel(invoice.lastSentAt || invoice.sentAt))}${invoice.sentBy ? ` by ${escapeHtml(invoice.sentBy)}` : ""}. Opens count when the reader's mail app loads images; some apps load them for privacy without the reader opening it.</p>` : ""}
      <table class="w-full text-xs"><tbody class="divide-y divide-border">${lines}</tbody><tfoot class="text-muted-foreground"><tr><td colspan="2" class="pt-2 text-right">Subtotal</td><td class="pt-2 text-right text-foreground">${escapeHtml(money(invoice.subtotal, invoice.currency))}</td></tr>${invoice.tax ? `<tr><td colspan="2" class="text-right">Tax (${invoice.taxRate / 100}%)</td><td class="text-right text-foreground">${escapeHtml(money(invoice.tax, invoice.currency))}</td></tr>` : ""}<tr><td colspan="2" class="text-right font-semibold text-foreground">Total</td><td class="text-right font-semibold text-foreground">${escapeHtml(money(invoice.total, invoice.currency))}</td></tr></tfoot></table>
      ${invoice.publicUrl ? `<div class="flex flex-wrap items-center gap-2"><input readonly value="${escapeHtml(invoice.publicUrl)}" aria-label="Invoice link" class="h-control-sm min-w-0 flex-1 rounded-md border border-border bg-background px-2 text-xs text-muted-foreground" /><button type="button" data-invoice-copy-link class="fm-btn fm-btn-sm">Copy link</button><a href="${escapeHtml(invoice.publicUrl)}" target="_blank" rel="noopener noreferrer" class="fm-btn fm-btn-sm">Preview</a></div><p class="text-xs text-muted-foreground">Opening the link yourself counts as a view.</p>` : ""}
      <section><h3 class="mb-2 text-xs font-semibold text-foreground">History</h3>${timeline(invoice.events)}</section>
    </div>`;
    const editable = ["draft", "sent"].includes(invoice.status);
    const buttons = [];
    if (["draft", "void"].includes(invoice.status)) buttons.push(`<button type="button" data-invoice-action="delete" class="fm-btn fm-btn-sm fm-btn-danger mr-auto">Delete</button>`);
    else if (invoice.status !== "void") buttons.push(`<button type="button" data-invoice-action="void" class="fm-btn fm-btn-sm fm-btn-danger mr-auto">Void</button>`);
    buttons.push(`<button type="button" data-invoice-action="duplicate" class="fm-btn fm-btn-sm">Duplicate</button>`);
    if (editable) buttons.push(`<button type="button" data-invoice-action="edit" class="fm-btn fm-btn-sm">Edit</button>`);
    if (editable && invoice.sentAt) buttons.push(`<button type="button" data-invoice-action="paid" class="fm-btn fm-btn-sm">Record payment</button>`);
    if (["paid", "void"].includes(invoice.status)) buttons.push(`<button type="button" data-invoice-action="reopen" class="fm-btn fm-btn-sm">Reopen</button>`);
    if (editable) buttons.push(`<button type="button" data-invoice-action="send" class="fm-btn fm-btn-sm"><i data-lucide="send"></i>${invoice.sentAt ? "Send reminder" : "Email invoice"}</button>`);
    if (invoice.status === "draft") buttons.push(`<button type="button" data-invoice-action="paid" class="fm-btn fm-btn-sm">Mark paid</button>`);
    actions.innerHTML = buttons.join("");
    icons();
  }
  async function openDetail(id) {
    state.detailId = id; setParam("invoice", id);
    const dialog = $(`[data-invoice-detail]`);
    const cached = byId(id); if (cached) renderDetail({ ...cached, events: null });
    if (!dialog.open) dialog.showModal();
    try { const payload = await request(orgPath(`/items/${id}`)); if (state.detailId === id) renderDetail(payload.invoice); }
    catch (error) { message("error", `Could not open the invoice: ${error.message}`); dialog.close(); }
  }
  function openSend(invoice) {
    $(`[data-invoice-send-title]`).textContent = invoice.sentAt ? `Remind about ${invoice.number}` : `Email ${invoice.number}`;
    $(`[data-invoice-send-to]`).value = invoice.client?.email || "";
    $(`[data-invoice-send-subject]`).value = "";
    $(`[data-invoice-send-subject]`).placeholder = `${invoice.sentAt ? "Reminder: invoice" : "Invoice"} ${invoice.number} from ${state.settings.businessName || state.organization}`;
    $(`[data-invoice-send-message]`).value = "";
    $(`[data-invoice-send-message]`).placeholder = "Leave empty for a short message with the amount and due date.";
    $(`[data-invoice-send-dialog]`).showModal();
  }
  async function submitSend(event) {
    event.preventDefault();
    const button = $(`[data-invoice-send-submit]`); button.disabled = true;
    try {
      const payload = await request(orgPath(`/items/${state.detailId}/send`), "POST", {
        to: $(`[data-invoice-send-to]`).value, subject: $(`[data-invoice-send-subject]`).value, message: $(`[data-invoice-send-message]`).value,
      });
      $(`[data-invoice-send-dialog]`).close(); message("notice", `Invoice ${payload.invoice.number} emailed to ${payload.invoice.sentTo}.`);
      await load(); renderDetail(payload.invoice);
    } catch (error) {
      const hint = error.message === "invalid sender" ? " Add a From email in Settings, or in the CRM's email settings." : "";
      message("error", `The invoice was not sent: ${error.message}.${hint}`);
    } finally { button.disabled = false; }
  }
  function openPaid(invoice) {
    $(`[data-invoice-paid-amount]`).value = ((invoice.balance || invoice.total) / 100).toFixed(2);
    $(`[data-invoice-paid-date]`).value = new Date().toISOString().slice(0, 10);
    $(`[data-invoice-paid-method]`).value = ""; $(`[data-invoice-paid-note]`).value = "";
    $(`[data-invoice-paid-dialog]`).showModal();
  }
  async function submitPaid(event) {
    event.preventDefault();
    try {
      const payload = await request(orgPath(`/items/${state.detailId}/paid`), "POST", {
        amount: cents($(`[data-invoice-paid-amount]`).value), paidAt: inputStamp($(`[data-invoice-paid-date]`).value),
        method: $(`[data-invoice-paid-method]`).value, note: $(`[data-invoice-paid-note]`).value,
      });
      $(`[data-invoice-paid-dialog]`).close();
      message("notice", payload.invoice.status === "paid" ? `Invoice ${payload.invoice.number} is paid.` : `Payment recorded; ${money(payload.invoice.balance, payload.invoice.currency)} still due.`);
      await load(); renderDetail(payload.invoice);
    } catch (error) { message("error", `The payment was not recorded: ${error.message}`); }
  }
  async function statusAction(action) {
    const invoice = byId(state.detailId); if (!invoice) return;
    if (action === "void" && !confirm(`Void invoice ${invoice.number}? Its link will show it as void.`)) return;
    try { const payload = await request(orgPath(`/items/${invoice.id}/${action}`), "POST", {}); await load(); renderDetail(payload.invoice); }
    catch (error) { message("error", `Could not ${action} the invoice: ${error.message}`); }
  }
  async function detailAction(action) {
    const invoice = byId(state.detailId); if (!invoice) return;
    if (action === "edit") return openEditor(invoice);
    if (action === "duplicate") return openEditor(null, { ...invoice, issuedAt: 0 });
    if (action === "send") return openSend(invoice);
    if (action === "paid") return openPaid(invoice);
    if (action === "delete") return removeInvoice(invoice.id);
    return statusAction(action);
  }

  function openSettings() {
    const settings = state.settings || {};
    $$(`[data-invoice-setting]`).forEach((node) => {
      const key = node.dataset.invoiceSetting; const value = settings[key];
      node.value = key === "taxRate" ? String((value || 0) / 100) : value ?? "";
      node.disabled = !canManage();
    });
    $(`[data-invoice-settings-save]`).classList.toggle("hidden", !canManage());
    $(`[data-invoice-settings-hint]`).textContent = canManage() ? "Shown on every invoice and invoice email." : "Only an organization owner or admin can change these.";
    $(`[data-invoice-settings-dialog]`).showModal();
  }
  async function saveSettings(event) {
    event.preventDefault();
    const body = {};
    $$(`[data-invoice-setting]`).forEach((node) => {
      const key = node.dataset.invoiceSetting;
      body[key] = key === "taxRate" ? Math.round((Number(node.value) || 0) * 100) : ["dueDays", "nextNumber"].includes(key) ? Number(node.value) : node.value;
    });
    try { const payload = await request(orgPath("/settings"), "PATCH", body); state.settings = payload.settings;
      $(`[data-invoice-settings-dialog]`).close(); message("notice", "Invoicing settings saved."); }
    catch (error) { message("error", `Settings were not saved: ${error.message}`); }
  }
  async function install() {
    try { await request(pluginPath("/install"), "POST", {}); selectedOrg().installed = true;
      message("notice", "Invoicing installed."); await load(); }
    catch (error) { message("error", `Could not install: ${error.message}`); }
  }
  async function exportAll() {
    try { const payload = await request(pluginPath("/export"));
      downloadJson(payload.export || {}, payload.filename || "forkmesh-invoices.json"); }
    catch (error) { message("error", `Export failed: ${error.message}`); }
  }
  async function uninstall() {
    if (!confirm(`Uninstall Invoicing from ${state.organization}? An export downloads first, then every invoice is removed and emailed links stop working.`)) return;
    try { const snapshot = await request(pluginPath("/export"));
      if (snapshot.hasData) downloadJson(snapshot.export || {}, snapshot.filename);
      await request(pluginPath("/uninstall"), "POST", {}); selectedOrg().installed = false;
      $(`[data-invoice-manage-dialog]`).close(); message("notice", "Invoicing uninstalled."); await load(); }
    catch (error) { message("error", `Could not uninstall: ${error.message}`); }
  }
  async function load() {
    renderInstall(); if (!selectedOrg()?.installed) return;
    try {
      const payload = await request(orgPath());
      state.invoices = payload.invoices || []; state.summary = payload.summary || {};
      state.activity = payload.activity || []; state.settings = payload.settings || {};
      if (state.crmInstalled !== Boolean(payload.crmInstalled)) state.contacts = null;
      state.crmInstalled = Boolean(payload.crmInstalled);
      message("error", ""); render();
    } catch (error) {
      if (error.status === 409) { selectedOrg().installed = false; renderInstall(); }
      else message("error", `Invoicing is unavailable: ${error.message}`);
    }
  }
  async function openRequested() {
    if (requested.invoice && byId(requested.invoice)) await openDetail(requested.invoice);
    else if (requested.contact && /^[0-9a-f]{32}$/.test(requested.contact) && state.crmInstalled) {
      await openEditor(null, { contactId: requested.contact }); setParam("contact", "");
    }
  }
  async function init() {
    if (!session()?.sessionToken) return message("error", "Sign in to use Invoicing.");
    try {
      const payload = await request("/api/invoices"); state.organizations = payload.organizations || [];
      if (!state.organizations.length) return message("error", "Join or create an organization before using Invoicing.");
      const select = $(`[data-invoice-organization]`);
      select.innerHTML = state.organizations.map((org) => `<option value="${escapeHtml(org.name)}">${escapeHtml(org.name)} · ${escapeHtml(org.role)}${org.installed ? "" : " · not installed"}</option>`).join("");
      const wanted = state.organizations.find((org) => org.name === requested.org)
        || state.organizations.find((org) => org.installed) || state.organizations[0];
      state.organization = wanted.name; select.value = state.organization;
      select.addEventListener("change", () => { state.organization = select.value; state.contacts = null; setParam("org", select.value); void load(); });
      await load(); await openRequested();
    } catch (error) { return message("error", `Invoicing is unavailable: ${error.message}`); }
    $(`[data-invoice-install]`)?.addEventListener("click", () => void install());
    $(`[data-invoice-new]`)?.addEventListener("click", () => void openEditor());
    $(`[data-invoice-settings-open]`)?.addEventListener("click", openSettings);
    $(`[data-invoice-settings-form]`)?.addEventListener("submit", saveSettings);
    $(`[data-invoice-manage]`)?.addEventListener("click", () => $(`[data-invoice-manage-dialog]`).showModal());
    $(`[data-invoice-export]`)?.addEventListener("click", () => void exportAll());
    $(`[data-invoice-uninstall]`)?.addEventListener("click", () => void uninstall());
    $(`[data-invoice-search]`)?.addEventListener("input", renderList);
    $(`[data-invoice-status-filter]`)?.addEventListener("change", renderList);
    for (const selector of [`[data-invoice-list]`, `[data-invoice-activity]`]) {
      $(selector)?.addEventListener("click", (event) => { const row = event.target.closest("[data-invoice-open]"); if (row) void openDetail(row.dataset.invoiceOpen); });
    }
    $(`[data-invoice-form]`)?.addEventListener("submit", saveEditor);
    $(`[data-invoice-delete]`)?.addEventListener("click", () => void removeInvoice($(`[data-invoice-id]`).value));
    $(`[data-invoice-contact]`)?.addEventListener("change", (event) => fillClientFromContact(event.target.value));
    $(`[data-invoice-line-add]`)?.addEventListener("click", () => { state.lines.push({ description: "", quantity: 1, unit: "" }); renderLines(); });
    $(`[data-invoice-lines]`)?.addEventListener("input", (event) => {
      const node = event.target.closest("[data-invoice-line]"); if (!node) return;
      const line = state.lines[Number(node.dataset.invoiceLine)]; if (line) { line[node.dataset.field] = node.value; renderTotals(); }
    });
    $(`[data-invoice-lines]`)?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-invoice-line-remove]"); if (!button) return;
      state.lines.splice(Number(button.dataset.invoiceLineRemove), 1);
      if (!state.lines.length) state.lines.push({ description: "", quantity: 1, unit: "" });
      renderLines();
    });
    $(`[data-invoice-tax]`)?.addEventListener("input", renderTotals);
    $(`[data-invoice-currency]`)?.addEventListener("input", renderTotals);
    $(`[data-invoice-detail-actions]`)?.addEventListener("click", (event) => { const button = event.target.closest("[data-invoice-action]"); if (button) void detailAction(button.dataset.invoiceAction); });
    $(`[data-invoice-detail-body]`)?.addEventListener("click", async (event) => {
      if (!event.target.closest("[data-invoice-copy-link]")) return;
      const url = byId(state.detailId)?.publicUrl; if (!url) return;
      try { await navigator.clipboard.writeText(url); message("notice", "Invoice link copied."); } catch { message("error", "The link could not be copied."); }
    });
    $(`[data-invoice-detail]`)?.addEventListener("close", () => setParam("invoice", ""));
    $(`[data-invoice-send-form]`)?.addEventListener("submit", submitSend);
    $(`[data-invoice-paid-form]`)?.addEventListener("submit", submitPaid);
    $$(`[data-invoice-dialog-close]`).forEach((button) => button.addEventListener("click", () => button.closest("dialog")?.close()));
  }
  return init();
});

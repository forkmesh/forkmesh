window.ForkMeshDashboard.registerPage("booking", ({ select: $, selectAll: $$, session, escapeHtml }) => {
  const DAYS = [["mon", "Monday"], ["tue", "Tuesday"], ["wed", "Wednesday"], ["thu", "Thursday"], ["fri", "Friday"], ["sat", "Saturday"], ["sun", "Sunday"]];
  const LOCATION_LABELS = { video: "Video call", phone: "Phone call", place: "In person", custom: "Other", none: "No location" };
  const LOCATION_HINTS = { video: "https://meet.example.com/you", phone: "Leave empty to ask the guest for their number", place: "Address", custom: "Shown after booking", none: "" };
  const ERRORS = {
    slug_taken: "That link is taken. Try another.", invalid_slug: "Links use lowercase letters, numbers and dashes.",
    slug_reserved: "That link is reserved. Try another.", meeting_type_required: "Keep at least one meeting type.",
    invalid_meeting_url: "A video call link must start with https://.", invalid_email: "Enter a valid email address.",
    booking_locked: "That meeting already happened or was cancelled.",
  };
  const state = { installed: false, data: null, tab: "upcoming", cancelling: null, focus: new URLSearchParams(location.search).get("booking") || "" };
  const localZone = Intl.DateTimeFormat().resolvedOptions().timeZone || "UTC";
  let slugTimer = 0;

  function message(kind, value = "") {
    const node = $(`[data-booking-${kind}]`); if (!node) return;
    const slot = node.querySelector("[data-booking-message-text]") || node;
    slot.textContent = value; node.classList.toggle("hidden", !value);
  }
  function explain(error) { return ERRORS[error.payload?.error] || error.message; }
  async function request(path, method = "GET", body = null) {
    const headers = { accept: "application/json" };
    if (session()?.sessionToken) headers.authorization = `Bearer ${session().sessionToken}`;
    if (body !== null) headers["content-type"] = "application/json";
    const response = await fetch(`/api/booking${path}`, { method, headers, cache: "no-store", body: body === null ? undefined : JSON.stringify(body) });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(String(payload.error || `HTTP ${response.status}`).replaceAll("_", " "));
      error.status = response.status; error.payload = payload; throw error;
    }
    return payload;
  }
  function downloadJson(payload, filename) {
    const url = URL.createObjectURL(new Blob([JSON.stringify(payload, null, 2)], { type: "application/json" }));
    const link = document.createElement("a"); link.href = url; link.download = filename; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  function minutesLabel(value) {
    const minutes = Number(value || 0);
    if (minutes < 60) return `${minutes} min`;
    const hours = Math.floor(minutes / 60), rest = minutes % 60;
    return rest ? `${hours} h ${rest} min` : `${hours} h`;
  }
  function noticeLabel(value) {
    const minutes = Number(value || 0);
    if (!minutes) return "Any time";
    if (minutes < 60) return `${minutes} minutes before`;
    if (minutes < 1440) return `${minutes / 60} hour${minutes === 60 ? "" : "s"} before`;
    return `${minutes / 1440} day${minutes === 1440 ? "" : "s"} before`;
  }
  function clock(ms) {
    return new Date(ms).toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit", hour12: state.data?.settings?.timeFormat !== "24h" });
  }
  function dayLabel(ms) { return new Date(ms).toLocaleDateString(undefined, { weekday: "long", day: "numeric", month: "long" }); }
  function dayKey(ms) { const d = new Date(ms); return `${d.getFullYear()}-${d.getMonth()}-${d.getDate()}`; }
  function options(list, current, label) { return list.map((value) => `<option value="${escapeHtml(String(value))}"${String(value) === String(current) ? " selected" : ""}>${escapeHtml(label(value))}</option>`).join(""); }
  function zones(current) {
    let list = [];
    try { list = Intl.supportedValuesOf("timeZone"); } catch (_) { list = []; }
    for (const extra of [current, localZone, "UTC"]) if (extra && !list.includes(extra)) list.push(extra);
    return list.sort();
  }

  function renderInstall() {
    $(`[data-booking-install-panel]`)?.classList.toggle("hidden", state.installed);
    $(`[data-booking-app]`)?.classList.toggle("hidden", !state.installed);
    $$(`[data-booking-requires-install]`).forEach((node) => node.classList.toggle("hidden", !state.installed));
  }

  function renderLink() {
    const data = state.data;
    const link = $(`[data-booking-link]`);
    link.href = data.link; link.textContent = data.link.replace(/^https?:\/\//, "");
    $(`[data-booking-open]`).href = data.link;
    const status = $(`[data-booking-status]`);
    status.textContent = data.enabled ? "Live" : "Paused";
    status.className = `fm-label ${data.enabled ? "fm-label-success" : "fm-label-attention"}`;
    $(`[data-booking-toggle]`).innerHTML = data.enabled ? `<i data-lucide="pause"></i>Pause page` : `<i data-lucide="play"></i>Turn page on`;
    const types = data.settings.types || [];
    $(`[data-booking-type-links]`).innerHTML = types.map((type) => `<li class="flex flex-wrap items-center justify-between gap-2 px-4 py-2">
        <span class="min-w-0 text-sm text-foreground"><span class="font-medium">${escapeHtml(type.title)}</span> <span class="text-muted-foreground">· ${escapeHtml(minutesLabel(type.minutes))}${type.enabled ? "" : " · hidden"}</span></span>
        <span class="flex items-center gap-2"><a class="truncate text-xs text-accent hover:underline" href="${escapeHtml(data.links[type.id] || "")}" target="_blank" rel="noopener">${escapeHtml(String(data.links[type.id] || "").replace(/^https?:\/\//, ""))}</a>
        <button type="button" class="fm-btn fm-btn-sm fm-btn-invisible" data-booking-copy-type="${escapeHtml(type.id)}" aria-label="Copy the link for ${escapeHtml(type.title)}"><i data-lucide="copy"></i></button></span></li>`).join("");
    for (const [key, value] of Object.entries(data.stats || {})) {
      const node = $(`[data-booking-stat="${key}"]`); if (node) node.textContent = Number(value || 0).toLocaleString();
    }
    const minutes = Number(data.stats?.bookedMinutes7Days || 0);
    $(`[data-booking-stat-hours]`).textContent = minutes ? `${minutesLabel(minutes)} booked` : "";
    const count = (data.upcoming || []).length;
    $(`[data-booking-upcoming-count]`).textContent = count ? String(count) : "";
  }

  function bookingRow(booking, past) {
    const guest = booking.guest || {};
    const where = booking.location?.kind === "phone" ? (booking.location.detail || guest.phone || "Phone call")
      : booking.location?.detail || LOCATION_LABELS[booking.location?.kind] || "";
    const cancelled = booking.status === "cancelled";
    const status = cancelled ? `<span class="fm-label fm-label-danger">Cancelled${booking.cancelledBy === "owner" ? " by you" : booking.cancelledBy === "guest" ? " by guest" : ""}</span>` : past ? `<span class="fm-label">Done</span>` : "";
    const moved = booking.rescheduledFrom ? `<span class="fm-label fm-label-accent">Moved</span>` : "";
    return `<li class="bkd-row px-4 py-3 ${booking.id === state.focus ? "bkd-highlight" : ""}" data-booking-row="${escapeHtml(booking.id)}">
      <div class="text-sm font-semibold text-foreground">${escapeHtml(`${clock(booking.startAt)} – ${clock(booking.endAt)}`)}<p class="text-xs font-normal text-muted-foreground">${escapeHtml(minutesLabel(booking.minutes))}</p></div>
      <div class="min-w-0">
        <p class="flex flex-wrap items-center gap-2 text-sm text-foreground"><span class="font-semibold">${escapeHtml(guest.name || "Guest")}</span><span class="text-muted-foreground">${escapeHtml(booking.typeTitle || "")}</span>${status}${moved}</p>
        <p class="mt-1 flex flex-wrap gap-x-3 text-xs text-muted-foreground"><a class="text-accent hover:underline" href="mailto:${escapeHtml(guest.email || "")}">${escapeHtml(guest.email || "")}</a>${guest.phone ? `<span>${escapeHtml(guest.phone)}</span>` : ""}${where ? `<span class="truncate">${escapeHtml(where)}</span>` : ""}${guest.timezone && guest.timezone !== state.data.settings.timezone ? `<span>Their time zone: ${escapeHtml(guest.timezone)}</span>` : ""}</p>
        ${guest.notes ? `<p class="mt-1 whitespace-pre-wrap break-words text-xs text-foreground">${escapeHtml(guest.notes)}</p>` : ""}
        ${cancelled && booking.cancelReason ? `<p class="mt-1 text-xs italic text-muted-foreground">“${escapeHtml(booking.cancelReason)}”</p>` : ""}
      </div>
      <div class="flex flex-wrap items-center gap-2">
        ${booking.calendarEventId && !cancelled ? `<a class="fm-btn fm-btn-sm" href="/dashboard/calendar?event=${escapeHtml(booking.calendarEventId)}"><i data-lucide="calendar"></i>Calendar</a>` : ""}
        ${!past && !cancelled ? `<button type="button" class="fm-btn fm-btn-sm fm-btn-danger" data-booking-cancel="${escapeHtml(booking.id)}">Cancel</button>` : ""}
      </div>
    </li>`;
  }

  function bookingList(list, past) {
    if (!list.length) {
      return past
        ? `<div class="fm-blankslate"><i data-lucide="history"></i><p class="fm-blankslate-title">Nothing here yet</p><p class="fm-blankslate-desc">Meetings from the last 90 days and cancelled ones show up here.</p></div>`
        : `<div class="fm-blankslate"><i data-lucide="calendar-check"></i><p class="fm-blankslate-title">No meetings booked yet</p><p class="fm-blankslate-desc">Send your link to someone. When they book, the meeting appears here and on your calendar.</p></div>`;
    }
    const groups = [];
    for (const booking of list) {
      const key = dayKey(booking.startAt);
      if (!groups.length || groups[groups.length - 1].key !== key) groups.push({ key, label: dayLabel(booking.startAt), items: [] });
      groups[groups.length - 1].items.push(booking);
    }
    return groups.map((group) => `<div><h3 class="border-b border-border bg-secondary px-4 py-1.5 text-xs font-semibold text-muted-foreground">${escapeHtml(group.label)}</h3>
      <ul class="divide-y divide-border">${group.items.map((booking) => bookingRow(booking, past || booking.endAt < Date.now())).join("")}</ul></div>`).join("");
  }

  function typeCard(type, index) {
    const location = type.location || { kind: "video", detail: "" };
    const durations = state.data.options.durations;
    return `<div class="bkd-type-card p-4" data-booking-type-row data-type-id="${escapeHtml(type.id || "")}">
      <div class="bkd-type-main">
        <label class="grid gap-1 text-xs text-muted-foreground">Name<input data-type-field="title" required maxlength="80" class="bkd-input" value="${escapeHtml(type.title || "")}"></label>
        <label class="grid gap-1 text-xs text-muted-foreground">Length<select data-type-field="minutes" class="bkd-input">${options(durations, type.minutes || 30, minutesLabel)}</select></label>
        <label class="bkd-span grid gap-1 text-xs text-muted-foreground">Description<textarea data-type-field="description" maxlength="600" rows="2" class="rounded-md border border-border bg-background px-2.5 py-2 text-control text-foreground">${escapeHtml(type.description || "")}</textarea></label>
      </div>
      <div class="grid content-start gap-3">
        <div class="bkd-type-where">
          <label class="grid gap-1 text-xs text-muted-foreground">Where<select data-type-field="kind" class="bkd-input">${options(state.data.options.locations, location.kind, (value) => LOCATION_LABELS[value] || value)}</select></label>
          <label class="grid gap-1 text-xs text-muted-foreground">Details<input data-type-field="detail" maxlength="500" class="bkd-input" value="${escapeHtml(location.detail || "")}" placeholder="${escapeHtml(LOCATION_HINTS[location.kind] || "")}"></label>
        </div>
        <div class="flex flex-wrap items-center justify-between gap-2">
          <label class="flex items-center gap-2 text-sm text-foreground"><input data-type-field="enabled" type="checkbox"${type.enabled !== false ? " checked" : ""}>Show on my page</label>
          <button type="button" class="fm-btn fm-btn-sm fm-btn-danger" data-booking-remove-type="${index}">Remove</button>
        </div>
      </div>
    </div>`;
  }

  function collectTypes() {
    return $$(`[data-booking-type-row]`).map((row) => {
      const field = (name) => row.querySelector(`[data-type-field="${name}"]`);
      return {
        id: row.dataset.typeId || "", title: field("title").value, minutes: Number(field("minutes").value),
        description: field("description").value, enabled: field("enabled").checked,
        location: { kind: field("kind").value, detail: field("detail").value },
      };
    });
  }

  function windowHtml(window) {
    const end = window[1] === "24:00" ? "23:59" : window[1];
    return `<span class="bkd-window" data-booking-window><input type="time" step="300" class="bkd-input" aria-label="From" value="${escapeHtml(window[0])}"><span class="text-muted-foreground">–</span><input type="time" step="300" class="bkd-input" aria-label="Until" value="${escapeHtml(end)}"><button type="button" class="fm-btn fm-btn-sm fm-btn-invisible fm-btn-icon" data-booking-remove-window aria-label="Remove these hours"><i data-lucide="x"></i></button></span>`;
  }
  function dayRow([day, label], windows) {
    const on = windows.length > 0;
    return `<div class="bkd-day-row" data-booking-day="${day}">
      <label class="flex h-8 items-center gap-2 text-sm font-medium text-foreground"><input type="checkbox" data-booking-day-on${on ? " checked" : ""}>${escapeHtml(label)}</label>
      <div class="flex flex-wrap items-center gap-2" data-booking-windows>${on ? windows.map(windowHtml).join("") : `<span class="flex h-8 items-center text-sm text-muted-foreground">Unavailable</span>`}${on ? `<button type="button" class="fm-btn fm-btn-sm fm-btn-invisible" data-booking-add-window><i data-lucide="plus"></i>Add hours</button>` : ""}</div>
    </div>`;
  }
  function collectWeek() {
    const result = {};
    for (const row of $$(`[data-booking-day]`)) {
      const on = row.querySelector("[data-booking-day-on]").checked;
      result[row.dataset.bookingDay] = on ? [...row.querySelectorAll("[data-booking-window]")].map((node) => {
        const [from, until] = node.querySelectorAll("input");
        return [from.value, until.value === "23:59" ? "24:00" : until.value];
      }).filter(([from, until]) => from && until) : [];
    }
    return result;
  }

  function renderForms() {
    const data = state.data, settings = data.settings, opts = data.options;
    $(`[data-booking-types]`).innerHTML = (settings.types || []).map(typeCard).join("");
    $(`[data-booking-week]`).innerHTML = DAYS.map((day) => dayRow(day, settings.availability?.[day[0]] || [])).join("");
    const field = (name) => $(`[data-booking-field="${name}"]`);
    field("timezone").innerHTML = zones(settings.timezone).map((zone) => `<option value="${escapeHtml(zone)}"${zone === settings.timezone ? " selected" : ""}>${escapeHtml(zone.replaceAll("_", " "))}</option>`).join("");
    field("minNoticeMinutes").innerHTML = options(opts.notices, settings.minNoticeMinutes, noticeLabel);
    field("maxDaysAhead").innerHTML = options(opts.horizons, settings.maxDaysAhead, (value) => `${value} days ahead`);
    field("bufferMinutes").innerHTML = options(opts.buffers, settings.bufferMinutes, (value) => value ? `${value} minutes` : "No gap");
    field("slotStepMinutes").innerHTML = options(opts.steps, settings.slotStepMinutes, (value) => value ? `${value} minutes` : "Meeting length");
    field("dailyLimit").value = settings.dailyLimit || "";
    for (const name of ["displayName", "headline", "welcome", "replyTo", "notifyEmail", "timeFormat"]) field(name).value = settings[name] || "";
    for (const name of ["checkCalendar", "emailOwner", "addToCalendar"]) field(name).checked = Boolean(settings[name]);
    field("slug").value = data.slug;
    $(`[data-booking-slug-prefix]`).textContent = `${data.origin.replace(/^https?:\/\//, "")}/meet?`;
    $(`[data-booking-slug-check]`).textContent = "";
    $(`[data-booking-accents]`).innerHTML = Object.entries(opts.accents).map(([name, color]) => `<label class="bkd-swatch" style="background:${escapeHtml(color)}" title="${escapeHtml(name)}"><input type="radio" name="booking-accent" value="${escapeHtml(name)}"${name === settings.accent ? " checked" : ""} aria-label="${escapeHtml(name)}"></label>`).join("");
  }

  function renderTabs() {
    $$(`[data-booking-tab]`).forEach((tab) => tab.setAttribute("aria-selected", tab.dataset.bookingTab === state.tab ? "true" : "false"));
    $$(`[data-booking-panel]`).forEach((panel) => panel.classList.toggle("hidden", panel.dataset.bookingPanel !== state.tab));
  }

  function render() {
    renderLink();
    $(`[data-booking-upcoming]`).innerHTML = bookingList(state.data.upcoming || [], false);
    $(`[data-booking-past]`).innerHTML = bookingList(state.data.past || [], true);
    renderForms();
    renderTabs();
    window.lucide?.createIcons?.();
  }

  async function load() {
    try {
      const payload = await request("");
      state.installed = payload.installed !== false;
      state.data = state.installed ? payload : null;
      message("error", "");
      renderInstall();
      if (!state.installed) {
        $(`[data-booking-install-slug]`).value = payload.suggestedSlug || "";
        $(`[data-booking-install-prefix]`).textContent = `${String(payload.origin || "https://forkmesh.com").replace(/^https?:\/\//, "")}/meet?`;
        return;
      }
      render();
      if (state.focus) {
        if ((state.data.past || []).some((item) => item.id === state.focus)) { state.tab = "past"; renderTabs(); }
        setTimeout(() => $(`[data-booking-row="${CSS.escape(state.focus)}"]`)?.scrollIntoView({ block: "center" }), 50);
      }
    } catch (error) {
      if (error.status === 409) { state.installed = false; renderInstall(); }
      else message("error", `Booking is unavailable: ${error.message}`);
    }
  }

  async function save(body, done) {
    try {
      state.data = await request("/page", "PUT", body);
      message("error", ""); message("notice", done); render();
    } catch (error) { message("notice", ""); message("error", `Could not save: ${explain(error)}`); }
  }

  async function install(event) {
    event.preventDefault();
    try {
      await request("/install", "POST", { slug: $(`[data-booking-install-slug]`).value.trim().toLowerCase(), timezone: localZone });
      message("notice", "Your booking page is live. Copy the link and send it to anyone.");
      await load();
    } catch (error) { message("error", `Could not create the page: ${explain(error)}`); }
  }

  async function copy(text, label) {
    try { await navigator.clipboard.writeText(text); }
    catch (_) { window.prompt("Copy this link", text); return; }
    if (label) { const before = label.textContent; label.textContent = "Copied"; setTimeout(() => { label.textContent = before; }, 1500); }
    else message("notice", "Link copied.");
  }

  function openCancel(id) {
    const booking = (state.data.upcoming || []).find((item) => item.id === id); if (!booking) return;
    state.cancelling = booking;
    $(`[data-booking-cancel-summary]`).textContent = `${booking.typeTitle} with ${booking.guest?.name || "your guest"}, ${dayLabel(booking.startAt)} at ${clock(booking.startAt)}.`;
    $(`[data-booking-cancel-reason]`).value = "";
    $(`[data-booking-cancel-notify]`).checked = true;
    $(`[data-booking-cancel-dialog]`).showModal();
  }
  async function confirmCancel(event) {
    event.preventDefault();
    const booking = state.cancelling; if (!booking) return;
    try {
      state.data = await request(`/bookings/${booking.id}/cancel`, "POST", { reason: $(`[data-booking-cancel-reason]`).value, notifyGuest: $(`[data-booking-cancel-notify]`).checked });
      $(`[data-booking-cancel-dialog]`).close();
      message("notice", $(`[data-booking-cancel-notify]`).checked ? `Cancelled. We emailed ${booking.guest?.name || "your guest"}.` : "Cancelled.");
      render();
    } catch (error) { message("error", `Could not cancel: ${explain(error)}`); }
  }

  async function exportMine() {
    try { const snapshot = await request("/export"); downloadJson(snapshot.export || {}, snapshot.filename || "forkmesh-booking.json"); }
    catch (error) { message("error", `Could not export: ${error.message}`); }
  }
  async function uninstall() {
    if (!confirm("Uninstall Booking? Your export downloads first, then your booking page and every booking are deleted and the link stops working.")) return;
    try {
      const snapshot = await request("/export");
      downloadJson(snapshot.export || {}, snapshot.filename || "forkmesh-booking.json");
      await request("/uninstall", "POST", {});
      $(`[data-booking-manage-dialog]`).close(); state.installed = false; state.data = null;
      message("notice", "Booking uninstalled."); await load();
    } catch (error) { message("error", `Could not uninstall: ${error.message}`); }
  }

  function checkSlug(value) {
    clearTimeout(slugTimer);
    const note = $(`[data-booking-slug-check]`);
    const slug = value.trim().toLowerCase();
    if (!slug || slug === state.data.slug) { note.textContent = ""; return; }
    slugTimer = setTimeout(async () => {
      try {
        const result = await request(`/slug/${encodeURIComponent(slug)}`);
        note.textContent = result.available ? "Available" : (ERRORS[result.reason] || "Not available");
        note.className = `text-xs ${result.available ? "text-success" : "text-destructive"}`;
      } catch (_) { note.textContent = ""; }
    }, 350);
  }

  async function init() {
    if (!session()?.sessionToken) return message("error", "Sign in to set up your booking page.");
    await load();
    $(`[data-booking-install-form]`)?.addEventListener("submit", install);
    $(`[data-booking-copy]`)?.addEventListener("click", () => void copy(state.data.link, $(`[data-booking-copy-label]`)));
    $(`[data-booking-toggle]`)?.addEventListener("click", () => void save({ enabled: !state.data.enabled }, state.data.enabled ? "Your page is paused. The link shows a paused notice until you turn it back on." : "Your page is live again."));
    $(`[data-booking-type-links]`)?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-booking-copy-type]"); if (button) void copy(state.data.links[button.dataset.bookingCopyType]);
    });
    $$(`[data-booking-tab]`).forEach((tab) => tab.addEventListener("click", () => { state.tab = tab.dataset.bookingTab; renderTabs(); }));
    $(`[data-booking-app]`)?.addEventListener("click", (event) => {
      const cancel = event.target.closest("[data-booking-cancel]");
      if (cancel) return openCancel(cancel.dataset.bookingCancel);
      const remove = event.target.closest("[data-booking-remove-type]");
      if (remove) {
        const types = collectTypes(); types.splice(Number(remove.dataset.bookingRemoveType), 1);
        $(`[data-booking-types]`).innerHTML = types.map(typeCard).join(""); window.lucide?.createIcons?.(); return;
      }
      if (event.target.closest("[data-booking-add-type]")) {
        const types = collectTypes(); types.push({ id: "", title: "New meeting", minutes: 30, description: "", enabled: true, location: { kind: "video", detail: "" } });
        $(`[data-booking-types]`).innerHTML = types.map(typeCard).join("");
        $$(`[data-booking-type-row]`).at(-1)?.querySelector("input")?.select(); window.lucide?.createIcons?.(); return;
      }
      const add = event.target.closest("[data-booking-add-window]");
      if (add) {
        add.insertAdjacentHTML("beforebegin", windowHtml(["13:00", "17:00"])); window.lucide?.createIcons?.(); return;
      }
      const drop = event.target.closest("[data-booking-remove-window]");
      if (drop) {
        const row = drop.closest("[data-booking-day]"); drop.closest("[data-booking-window]").remove();
        if (!row.querySelector("[data-booking-window]")) { row.outerHTML = dayRow(DAYS.find(([day]) => day === row.dataset.bookingDay), []); window.lucide?.createIcons?.(); }
      }
    });
    $(`[data-booking-app]`)?.addEventListener("change", (event) => {
      const toggle = event.target.closest("[data-booking-day-on]");
      if (toggle) {
        const row = toggle.closest("[data-booking-day]");
        row.outerHTML = dayRow(DAYS.find(([day]) => day === row.dataset.bookingDay), toggle.checked ? [["09:00", "17:00"]] : []);
        window.lucide?.createIcons?.(); return;
      }
      const kind = event.target.closest(`[data-type-field="kind"]`);
      if (kind) kind.closest("[data-booking-type-row]").querySelector(`[data-type-field="detail"]`).placeholder = LOCATION_HINTS[kind.value] || "";
    });
    $(`[data-booking-field="slug"]`)?.addEventListener("input", (event) => checkSlug(event.target.value));
    $(`[data-booking-types-form]`)?.addEventListener("submit", (event) => { event.preventDefault(); void save({ types: collectTypes() }, "Meeting types saved."); });
    $(`[data-booking-hours-form]`)?.addEventListener("submit", (event) => {
      event.preventDefault();
      const field = (name) => $(`[data-booking-field="${name}"]`);
      void save({
        availability: collectWeek(), timezone: field("timezone").value,
        minNoticeMinutes: Number(field("minNoticeMinutes").value), maxDaysAhead: Number(field("maxDaysAhead").value),
        bufferMinutes: Number(field("bufferMinutes").value), slotStepMinutes: Number(field("slotStepMinutes").value),
        dailyLimit: Number(field("dailyLimit").value || 0), checkCalendar: field("checkCalendar").checked,
      }, "Hours saved.");
    });
    $(`[data-booking-page-form]`)?.addEventListener("submit", (event) => {
      event.preventDefault();
      const field = (name) => $(`[data-booking-field="${name}"]`);
      void save({
        slug: field("slug").value.trim().toLowerCase(), displayName: field("displayName").value, headline: field("headline").value,
        welcome: field("welcome").value, timeFormat: field("timeFormat").value,
        accent: $(`input[name="booking-accent"]:checked`)?.value || "blue",
        replyTo: field("replyTo").value.trim(), notifyEmail: field("notifyEmail").value.trim(),
        emailOwner: field("emailOwner").checked, addToCalendar: field("addToCalendar").checked,
      }, "Page saved.");
    });
    $(`[data-booking-cancel-form]`)?.addEventListener("submit", confirmCancel);
    $(`[data-booking-manage]`)?.addEventListener("click", () => $(`[data-booking-manage-dialog]`).showModal());
    $(`[data-booking-export]`)?.addEventListener("click", () => void exportMine());
    $(`[data-booking-uninstall]`)?.addEventListener("click", () => void uninstall());
    $$(`[data-booking-dialog-close]`).forEach((button) => button.addEventListener("click", () => button.closest("dialog")?.close()));
    window.lucide?.createIcons?.();
  }
  return init();
});

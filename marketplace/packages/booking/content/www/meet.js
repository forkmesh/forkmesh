(() => {
  const API = "/api/booking/-";
  const ACCENTS = { blue: "#2563eb", violet: "#7c3aed", teal: "#0d9488", rose: "#e11d48", amber: "#d97706", slate: "#475569" };
  const SLUG_RE = /^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/;
  const TYPE_RE = /^[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?$/;
  const TOKEN_RE = /^[0-9a-f]{32}\.[0-9a-f]{24}$/;
  const DAY_MS = 86400000;
  const ICONS = {
    clock: '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/></svg>',
    video: '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="3" y="6" width="13" height="12" rx="2.5"/><path d="m16 10.5 5-3v9l-5-3"/></svg>',
    phone: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M5 4h3l2 5-2.5 1.5a11 11 0 0 0 6 6L15 14l5 2v3a2 2 0 0 1-2 2A16 16 0 0 1 3 6a2 2 0 0 1 2-2"/></svg>',
    place: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 21s-7-6.2-7-11.5a7 7 0 0 1 14 0C19 14.8 12 21 12 21z"/><circle cx="12" cy="9.5" r="2.5"/></svg>',
    custom: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 6h16M4 12h16M4 18h10"/></svg>',
    globe: '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"/></svg>',
    calendar: '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="3.5" y="5" width="17" height="15" rx="2.5"/><path d="M3.5 10h17M8 3v4M16 3v4"/></svg>',
    left: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m15 6-6 6 6 6"/></svg>',
    right: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m9 6 6 6-6 6"/></svg>',
    check: '<svg viewBox="0 0 52 52" aria-hidden="true"><circle class="bk-check-ring" cx="26" cy="26" r="24"/><path class="bk-check-tick" d="m15 27 7 7 15-16"/></svg>',
    download: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 4v11m0 0-4-4m4 4 4-4M5 20h14"/></svg>',
    copy: '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="8" y="8" width="12" height="12" rx="2.5"/><path d="M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h2"/></svg>',
  };
  const ERRORS = {
    page_not_found: "This booking link doesn't exist. Check the link with the person who sent it.",
    page_paused: "This booking page is paused right now. Try again later or contact the person who sent it.",
    meeting_type_not_found: "That meeting type isn't offered any more. Pick another below.",
    slot_unavailable: "Someone just took that time. Please pick another.",
    too_many_bookings: "You already have the most upcoming meetings this page allows. Cancel one first to book another.",
    rate_limited: "Too many bookings from this connection. Please try again in an hour.",
    page_busy: "This page has taken all the bookings it can today. Please try again tomorrow.",
    invalid_email: "Enter a valid email address so we can send you the details.",
    name_required: "Enter your name.",
    phone_required: "Enter the phone number to call you on.",
    invalid_phone: "That phone number doesn't look right.",
    booking_not_found: "We couldn't find this booking. The link may be incomplete, or the host removed their booking page.",
    booking_locked: "This meeting can't be changed any more.",
    meeting_type_unavailable: "This meeting type isn't offered any more, so it can't be moved. You can still cancel it.",
  };

  const root = document.querySelector("[data-bk-root]");
  const zone = Intl.DateTimeFormat().resolvedOptions().timeZone || "UTC";
  const state = {
    slug: "", typeId: "", token: "", page: null, type: null, booking: null,
    tz: zone, fmt: "12h", view: "loading", mode: "book",
    month: null, slots: new Map(), loadingMonth: "", day: "", slot: 0,
    error: "", notice: "", busy: false, form: { name: "", email: "", phone: "", notes: "" },
    jumped: false, bookableUntil: 0,
  };

  function h(tag, props, ...children) {
    const node = document.createElement(tag);
    for (const [key, value] of Object.entries(props || {})) {
      if (value === false || value === null || value === undefined) continue;
      if (key === "class") node.className = value;
      else if (key === "icon") node.innerHTML = ICONS[value] || "";
      else if (key.startsWith("on")) node.addEventListener(key.slice(2), value);
      else if (key === "text") node.textContent = value;
      else node.setAttribute(key, value === true ? "" : String(value));
    }
    for (const child of children.flat()) {
      if (child === null || child === undefined || child === false) continue;
      node.append(child instanceof Node ? child : document.createTextNode(String(child)));
    }
    return node;
  }
  function icon(name, cls = "bk-icon") { return h("span", { class: cls, icon: name }); }

  function parseAddress() {
    const params = new URLSearchParams(location.search);
    if (TOKEN_RE.test(params.get("b") || "")) return { token: params.get("b") };
    if (params.get("u")) return { slug: String(params.get("u")).toLowerCase(), typeId: String(params.get("t") || "").toLowerCase() };
    let raw = location.search.slice(1).split(/[&#=]/)[0] || "";
    try { raw = decodeURIComponent(raw); } catch (_) { raw = ""; }
    const [slug = "", typeId = ""] = raw.trim().toLowerCase().split("/");
    return { slug, typeId };
  }

  async function api(path, method = "GET", body = null) {
    const options = { method, headers: { accept: "application/json" }, credentials: "omit", cache: "no-store" };
    if (body) { options.headers["content-type"] = "application/json"; options.body = JSON.stringify(body); }
    const response = await fetch(API + path, options);
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(ERRORS[payload.error] || "Something went wrong. Please try again.");
      error.code = payload.error || "http_" + response.status;
      throw error;
    }
    return payload;
  }

  function dayKey(ms, tz = state.tz) {
    const parts = new Intl.DateTimeFormat("en-CA", { timeZone: tz, year: "numeric", month: "2-digit", day: "2-digit" }).formatToParts(new Date(ms));
    const pick = (type) => parts.find((part) => part.type === type)?.value || "";
    return `${pick("year")}-${pick("month")}-${pick("day")}`;
  }
  function monthOf(ms) { const [y, m] = dayKey(ms).split("-").map(Number); return { y, m: m - 1 }; }
  function monthId(month) { return `${month.y}-${String(month.m + 1).padStart(2, "0")}`; }
  function shiftMonth(month, delta) { const total = month.y * 12 + month.m + delta; return { y: Math.floor(total / 12), m: total % 12 }; }
  function clock(ms, tz = state.tz) {
    return new Intl.DateTimeFormat(undefined, { timeZone: tz, hour: "numeric", minute: "2-digit", hour12: state.fmt === "12h" }).format(new Date(ms));
  }
  function longDay(ms, tz = state.tz) {
    return new Intl.DateTimeFormat(undefined, { timeZone: tz, weekday: "long", day: "numeric", month: "long", year: "numeric" }).format(new Date(ms));
  }
  function keyLabel(key, options) {
    const [y, m, d] = key.split("-").map(Number);
    return new Intl.DateTimeFormat(undefined, { timeZone: "UTC", ...options }).format(new Date(Date.UTC(y, m - 1, d, 12)));
  }
  function minutesLabel(minutes) {
    if (minutes < 60) return `${minutes} min`;
    const hours = Math.floor(minutes / 60), rest = minutes % 60;
    return rest ? `${hours} h ${rest} min` : `${hours} hour${hours > 1 ? "s" : ""}`;
  }
  function zoneLabel(tz) {
    try {
      const name = new Intl.DateTimeFormat(undefined, { timeZone: tz, timeZoneName: "shortOffset" }).formatToParts(new Date()).find((part) => part.type === "timeZoneName")?.value || "";
      return `${tz.replaceAll("_", " ")}${name ? ` (${name})` : ""}`;
    } catch (_) { return tz; }
  }
  function zones() {
    let list = [];
    try { list = Intl.supportedValuesOf("timeZone"); } catch (_) { list = []; }
    for (const extra of [state.tz, zone, state.page?.timezone, "UTC"]) if (extra && !list.includes(extra)) list.push(extra);
    return list.sort();
  }

  function applyAccent(name) {
    const color = ACCENTS[name] || ACCENTS.blue;
    document.documentElement.style.setProperty("--bk-accent", color);
    document.querySelector('meta[name="theme-color"]')?.setAttribute("content", color);
  }

  function monthRange(month) {
    return { start: Date.UTC(month.y, month.m, 1) - 15 * 3600000, end: Date.UTC(month.y, month.m + 1, 1) + 15 * 3600000 };
  }
  function slotPath() {
    return state.mode === "reschedule" ? `/b/${state.token}/slots` : `/p/${state.slug}/${state.type.id}/slots`;
  }
  async function loadMonth(month) {
    const id = `${state.mode}:${state.type?.id}:${monthId(month)}`;
    if (state.slots.has(id)) return state.slots.get(id);
    state.loadingMonth = id;
    const range = monthRange(month);
    const payload = await api(`${slotPath()}?start=${range.start}&end=${range.end}`);
    state.bookableUntil = Number(payload.bookableUntil || 0);
    const byDay = new Map();
    for (const slot of payload.slots || []) {
      const key = dayKey(slot);
      if (!key.startsWith(monthId(month))) continue;
      if (!byDay.has(key)) byDay.set(key, []);
      byDay.get(key).push(slot);
    }
    state.slots.set(id, byDay);
    if (state.loadingMonth === id) state.loadingMonth = "";
    return byDay;
  }
  function monthSlots() { return state.slots.get(`${state.mode}:${state.type?.id}:${monthId(state.month)}`); }

  async function openCalendar() {
    state.view = "calendar"; state.day = ""; state.slot = 0; state.error = "";
    state.month = monthOf(Date.now()); state.jumped = false;
    render();
    await showMonth(state.month);
  }
  async function showMonth(month) {
    state.month = month; state.day = ""; state.slot = 0;
    render();
    try {
      const byDay = await loadMonth(month);
      if (!byDay.size && !state.jumped) {
        const next = shiftMonth(month, 1);
        if (state.bookableUntil && Date.UTC(next.y, next.m, 1) < state.bookableUntil) {
          state.jumped = true;
          const nextDays = await loadMonth(next);
          if (nextDays.size) state.month = next;
        }
      }
      state.jumped = true;
      if (window.matchMedia("(min-width: 960px)").matches) {
        const first = [...(monthSlots() || new Map()).keys()].sort()[0];
        if (first) state.day = first;
      }
    } catch (error) { state.error = error.message; }
    render();
  }

  function setAddress(type) {
    const query = type ? `?${state.slug}/${type.id}` : `?${state.slug}`;
    try { history.replaceState(null, "", location.pathname + query); } catch (_) { }
  }
  function chooseType(type) {
    state.type = type; state.typeId = type.id; setAddress(type);
    void openCalendar();
  }
  function backToTypes() {
    state.type = null; state.typeId = ""; state.view = "types"; state.error = ""; setAddress(null); render();
  }

  async function submitBooking(event) {
    event.preventDefault();
    if (state.busy) return;
    const form = event.currentTarget;
    const data = Object.fromEntries(new FormData(form).entries());
    state.form = { name: data.name || "", email: data.email || "", phone: data.phone || "", notes: data.notes || "" };
    state.busy = true; state.error = ""; render();
    try {
      const payload = await api(`/p/${state.slug}/${state.type.id}/book`, "POST", {
        name: data.name, email: data.email, phone: data.phone || "", notes: data.notes || "",
        website: data.website || "", startAt: state.slot, timezone: state.tz,
      });
      state.booking = payload.booking; state.token = payload.booking.token; state.view = "done";
      state.notice = payload.emailed ? `We emailed the details to ${data.email}.` : "Your meeting is booked. The confirmation email could not be sent, so keep this page or add it to your calendar now.";
    } catch (error) {
      state.error = error.message;
      if (error.code === "slot_unavailable") { state.slots.clear(); state.view = "calendar"; await showMonth(state.month); }
    } finally { state.busy = false; render(); }
  }
  async function confirmReschedule() {
    if (state.busy) return;
    state.busy = true; state.error = ""; render();
    try {
      const payload = await api(`/b/${state.token}/reschedule`, "POST", { startAt: state.slot });
      state.booking = payload.booking; state.mode = "book"; state.view = "manage";
      state.notice = "Your meeting has moved. We emailed you and your host the new time.";
    } catch (error) {
      state.error = error.message;
      if (error.code === "slot_unavailable") { state.slots.clear(); await showMonth(state.month); }
    } finally { state.busy = false; render(); }
  }
  async function cancelBooking(event) {
    event.preventDefault();
    if (state.busy) return;
    const reason = new FormData(event.currentTarget).get("reason") || "";
    state.busy = true; state.error = ""; render();
    try {
      const payload = await api(`/b/${state.token}/cancel`, "POST", { reason });
      state.booking = payload.booking; state.view = "manage"; state.notice = "Your meeting is cancelled. We let your host know.";
    } catch (error) { state.error = error.message; }
    finally { state.busy = false; render(); }
  }

  function utcStamp(ms) { return new Date(ms).toISOString().replace(/[-:]/g, "").replace(/\.\d{3}/, ""); }
  function manageUrl() { return `${location.origin}${location.pathname}?b=${state.booking.token}`; }
  function calendarLinks() {
    const booking = state.booking;
    const details = `Booked with ForkMesh. Reschedule or cancel: ${manageUrl()}`;
    const where = booking.location?.url || booking.location?.text || "";
    const google = `https://calendar.google.com/calendar/render?action=TEMPLATE&text=${encodeURIComponent(booking.title)}&dates=${utcStamp(booking.startAt)}/${utcStamp(booking.endAt)}&details=${encodeURIComponent(details)}&location=${encodeURIComponent(where)}`;
    const outlook = `https://outlook.live.com/calendar/0/deeplink/compose?path=%2Fcalendar%2Faction%2Fcompose&rru=addevent&subject=${encodeURIComponent(booking.title)}&startdt=${encodeURIComponent(new Date(booking.startAt).toISOString())}&enddt=${encodeURIComponent(new Date(booking.endAt).toISOString())}&body=${encodeURIComponent(details)}&location=${encodeURIComponent(where)}`;
    return { google, outlook, details, where };
  }
  function downloadIcs() {
    const booking = state.booking, links = calendarLinks();
    const escapeIcs = (value) => String(value || "").replace(/\\/g, "\\\\").replace(/\n/g, "\\n").replace(/[,;]/g, (match) => "\\" + match);
    const body = [
      "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//ForkMesh//Booking Page//EN", "METHOD:PUBLISH", "BEGIN:VEVENT",
      `UID:${booking.token.split(".")[0]}@booking.forkmesh`, `DTSTAMP:${utcStamp(Date.now())}`,
      `DTSTART:${utcStamp(booking.startAt)}`, `DTEND:${utcStamp(booking.endAt)}`,
      `SUMMARY:${escapeIcs(booking.title)}`, `DESCRIPTION:${escapeIcs(links.details)}`,
      links.where ? `LOCATION:${escapeIcs(links.where)}` : "", `URL:${manageUrl()}`,
      booking.status === "cancelled" ? "STATUS:CANCELLED" : "STATUS:CONFIRMED",
      "END:VEVENT", "END:VCALENDAR",
    ].filter(Boolean).join("\r\n");
    const url = URL.createObjectURL(new Blob([body], { type: "text/calendar" }));
    const link = h("a", { href: url, download: "meeting.ics" }); document.body.append(link); link.click(); link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  async function copyLink(button) {
    try { await navigator.clipboard.writeText(manageUrl()); button.querySelector("[data-label]").textContent = "Copied"; }
    catch (_) { window.prompt("Copy this link", manageUrl()); }
  }

  function locationIcon(kind) { return ICONS[kind] ? kind : "custom"; }
  function meta(iconName, text, cls = "") { return text ? h("li", { class: `bk-meta-row ${cls}` }, icon(iconName), h("span", { text })) : null; }

  function hostPanel() {
    const page = state.page || state.booking?.page || {};
    const type = state.type;
    const chosen = state.slot && (state.view === "form" || state.view === "calendar") ? state.slot : 0;
    const booking = state.booking;
    const showBack = (state.view === "calendar" || state.view === "form") && state.mode === "book" && (page.types || []).length > 1;
    return h("aside", { class: "bk-host" },
      showBack ? h("button", { type: "button", class: "bk-back", onclick: backToTypes, "aria-label": "All meeting types" }, icon("left"), "All meeting types") : null,
      h("div", { class: "bk-avatar", "aria-hidden": "true", text: page.initials || "?" }),
      h("p", { class: "bk-host-name", text: page.name || "" }),
      type ? h("h1", { class: "bk-title", text: type.title })
        : booking && (state.view === "manage" || state.view === "cancel") ? h("h1", { class: "bk-title", text: booking.typeTitle })
          : h("h1", { class: "bk-title", text: page.headline || "Book a meeting" }),
      type || booking ? h("ul", { class: "bk-meta" },
        type ? meta("clock", minutesLabel(type.minutes)) : null,
        type ? meta(locationIcon(type.location?.kind), type.location?.label) : null,
        chosen ? meta("calendar", `${clock(chosen)} – ${clock(chosen + type.minutes * 60000)}, ${longDay(chosen)}`, "bk-meta-strong") : null,
        meta("globe", zoneLabel(state.tz))) : null,
      type?.description ? h("p", { class: "bk-desc", text: type.description }) : null,
      !type && !booking && page.welcome ? h("p", { class: "bk-desc", text: page.welcome }) : null);
  }

  function typesPanel() {
    const page = state.page;
    const types = page.types || [];
    return h("section", { class: "bk-panel bk-animate" },
      h("h2", { class: "bk-h2", text: "Choose a meeting" }),
      types.length ? h("ul", { class: "bk-types" }, types.map((type) => h("li", {},
        h("button", { type: "button", class: "bk-type", onclick: () => chooseType(type) },
          h("span", { class: "bk-type-dot", "aria-hidden": "true" }),
          h("span", { class: "bk-type-body" },
            h("span", { class: "bk-type-title", text: type.title }),
            h("span", { class: "bk-type-meta" }, icon("clock", "bk-icon bk-icon-sm"), minutesLabel(type.minutes),
              type.location?.label ? h("span", { class: "bk-sep", "aria-hidden": "true", text: "·" }) : null,
              type.location?.label ? icon(locationIcon(type.location.kind), "bk-icon bk-icon-sm") : null,
              type.location?.label || ""),
            type.description ? h("span", { class: "bk-type-desc", text: type.description }) : null),
          icon("right", "bk-icon bk-chevron")))))
        : h("p", { class: "bk-empty", text: "There are no meetings to book here right now." }));
  }

  function calendarPanel() {
    const month = state.month;
    const byDay = monthSlots();
    const loading = !byDay;
    const today = dayKey(Date.now());
    const first = new Date(Date.UTC(month.y, month.m, 1));
    const lead = (first.getUTCDay() + 6) % 7;
    const count = new Date(Date.UTC(month.y, month.m + 1, 0)).getUTCDate();
    const current = monthOf(Date.now());
    const canBack = month.y * 12 + month.m > current.y * 12 + current.m;
    const next = shiftMonth(month, 1);
    const canNext = !state.bookableUntil || Date.UTC(next.y, next.m, 1) < state.bookableUntil + DAY_MS;
    const cells = [];
    for (let i = 0; i < lead; i += 1) cells.push(h("span", { class: "bk-day bk-day-pad", "aria-hidden": "true" }));
    for (let d = 1; d <= count; d += 1) {
      const key = `${monthId(month)}-${String(d).padStart(2, "0")}`;
      const open = byDay?.get(key)?.length || 0;
      cells.push(h("button", {
        type: "button",
        class: `bk-day${open ? " bk-day-open" : ""}${key === state.day ? " bk-day-selected" : ""}${key === today ? " bk-day-today" : ""}`,
        disabled: !open,
        "aria-pressed": key === state.day ? "true" : "false",
        "aria-label": `${keyLabel(key, { weekday: "long", day: "numeric", month: "long" })}${open ? `, ${open} open time${open > 1 ? "s" : ""}` : ", no open times"}`,
        onclick: () => { state.day = key; state.slot = 0; state.error = ""; render(); root.querySelector(".bk-times")?.scrollIntoView({ block: "nearest", behavior: "smooth" }); },
      }, h("span", { text: String(d) })));
    }
    const weekdays = [0, 1, 2, 3, 4, 5, 6].map((index) => h("span", { class: "bk-weekday", text: keyLabel(`2026-11-${String(2 + index).padStart(2, "0")}`, { weekday: "short" }) }));
    const monthTitle = new Intl.DateTimeFormat(undefined, { timeZone: "UTC", month: "long", year: "numeric" }).format(first);
    const daySlots = state.day ? byDay?.get(state.day) || [] : [];
    const none = byDay && !byDay.size;
    return h("section", { class: "bk-panel bk-calendar-panel bk-animate" },
      h("div", { class: "bk-cal" },
        h("h2", { class: "bk-h2", text: state.mode === "reschedule" ? "Pick a new time" : "Select a date & time" }),
        h("div", { class: "bk-month" },
          h("button", { type: "button", class: "bk-nav", disabled: !canBack, "aria-label": "Previous month", onclick: () => showMonth(shiftMonth(month, -1)) }, icon("left")),
          h("p", { class: "bk-month-title", text: monthTitle }),
          h("button", { type: "button", class: "bk-nav", disabled: !canNext, "aria-label": "Next month", onclick: () => showMonth(next) }, icon("right"))),
        h("div", { class: "bk-grid" + (loading ? " bk-grid-loading" : "") }, weekdays, cells),
        none ? h("p", { class: "bk-hint", text: "No open times this month." }) : null,
        h("div", { class: "bk-prefs" },
          h("label", { class: "bk-zone" }, icon("globe", "bk-icon bk-icon-sm"),
            h("span", { class: "bk-sr", text: "Time zone" }),
            h("select", { onchange: (event) => { state.tz = event.target.value; state.slots.clear(); void showMonth(state.month); } },
              zones().map((tz) => h("option", { value: tz, selected: tz === state.tz }, zoneLabel(tz))))),
          h("div", { class: "bk-toggle", role: "group", "aria-label": "Clock" },
            ["12h", "24h"].map((fmt) => h("button", { type: "button", "aria-pressed": state.fmt === fmt ? "true" : "false", onclick: () => { state.fmt = fmt; render(); } }, fmt))))),
      h("div", { class: "bk-times" + (state.day ? " bk-times-open" : "") },
        state.day ? h("p", { class: "bk-times-day", text: keyLabel(state.day, { weekday: "long", day: "numeric", month: "long" }) }) : null,
        state.day ? h("ul", { class: "bk-slot-list" }, daySlots.map((slot) => h("li", { class: slot === state.slot ? "bk-slot-chosen" : "" },
          h("button", { type: "button", class: "bk-slot", "aria-pressed": slot === state.slot ? "true" : "false", onclick: () => { state.slot = slot; state.error = ""; render(); } }, clock(slot)),
          slot === state.slot ? h("button", {
            type: "button", class: "bk-slot-next", disabled: state.busy,
            onclick: () => { if (state.mode === "reschedule") void confirmReschedule(); else { state.view = "form"; render(); root.querySelector("input[name=name]")?.focus(); } },
          }, state.mode === "reschedule" ? (state.busy ? "Moving…" : "Confirm") : "Next") : null)))
          : h("p", { class: "bk-hint bk-times-hint", text: loading ? "Finding open times…" : "Pick a day to see open times." })));
  }

  function field(label, name, props = {}, hint = "") {
    const control = props.multiline
      ? h("textarea", { name, rows: 4, maxlength: 2000, placeholder: props.placeholder || "" }, state.form[name] || "")
      : h("input", { name, type: props.type || "text", required: props.required, autocomplete: props.autocomplete, maxlength: props.maxlength || 120, value: state.form[name] || "", placeholder: props.placeholder || "", inputmode: props.inputmode });
    return h("label", { class: "bk-field" }, h("span", { class: "bk-label" }, label, props.required ? null : h("span", { class: "bk-optional", text: " (optional)" })), control, hint ? h("span", { class: "bk-field-hint", text: hint }) : null);
  }

  function formPanel() {
    const type = state.type;
    return h("section", { class: "bk-panel bk-form-panel bk-animate" },
      h("button", { type: "button", class: "bk-link-back", onclick: () => { state.view = "calendar"; render(); } }, icon("left", "bk-icon bk-icon-sm"), "Change time"),
      h("h2", { class: "bk-h2", text: "Your details" }),
      h("form", { class: "bk-form", onsubmit: submitBooking, novalidate: false },
        field("Name", "name", { required: true, autocomplete: "name" }),
        field("Email", "email", { required: true, type: "email", autocomplete: "email", maxlength: 254 }, "We'll send the confirmation here."),
        type.askPhone ? field("Phone number", "phone", { required: true, type: "tel", autocomplete: "tel", maxlength: 32, inputmode: "tel" }, "The number to call you on.") : null,
        field("Anything to share before we meet?", "notes", { multiline: true, placeholder: "Agenda, questions, links…" }),
        h("label", { class: "bk-trap", "aria-hidden": "true" }, "Website", h("input", { name: "website", tabindex: "-1", autocomplete: "off" })),
        h("button", { type: "submit", class: "bk-primary", disabled: state.busy }, state.busy ? h("span", { class: "bk-spinner bk-spinner-sm", "aria-hidden": "true" }) : null, state.busy ? "Booking…" : "Schedule meeting"),
        h("p", { class: "bk-fine", text: "Your name, email and notes go only to the person you are booking with. ForkMesh emails you the confirmation and a link to change it." })));
  }

  function summary(booking) {
    const tz = state.tz;
    return h("dl", { class: "bk-summary" },
      h("div", {}, h("dt", {}, icon("calendar", "bk-icon bk-icon-sm"), "When"),
        h("dd", {}, h("strong", { text: `${clock(booking.startAt, tz)} – ${clock(booking.endAt, tz)}` }), h("span", { text: longDay(booking.startAt, tz) }))),
      h("div", {}, h("dt", {}, icon("clock", "bk-icon bk-icon-sm"), "What"), h("dd", { text: `${booking.typeTitle}, ${minutesLabel(booking.minutes)}` })),
      booking.location?.text ? h("div", {}, h("dt", {}, icon(locationIcon(booking.location.kind), "bk-icon bk-icon-sm"), "Where"),
        h("dd", {}, booking.location.url ? h("a", { href: booking.location.url, rel: "noopener noreferrer", target: "_blank", text: booking.location.url }) : booking.location.text)) : null,
      h("div", {}, h("dt", {}, icon("globe", "bk-icon bk-icon-sm"), "Time zone"), h("dd", { text: zoneLabel(tz) })));
  }

  function calendarButtons() {
    const links = calendarLinks();
    return h("div", { class: "bk-add" },
      h("p", { class: "bk-add-title", text: "Add to your calendar" }),
      h("div", { class: "bk-add-row" },
        h("a", { class: "bk-chip", href: links.google, target: "_blank", rel: "noopener noreferrer" }, "Google"),
        h("a", { class: "bk-chip", href: links.outlook, target: "_blank", rel: "noopener noreferrer" }, "Outlook"),
        h("button", { type: "button", class: "bk-chip", onclick: downloadIcs }, icon("download", "bk-icon bk-icon-sm"), "Apple / .ics")));
  }

  function donePanel() {
    const booking = state.booking;
    return h("section", { class: "bk-panel bk-done bk-animate" },
      h("div", { class: "bk-check" + (state.view === "done" ? " bk-check-animate" : ""), icon: "check" }),
      h("h2", { class: "bk-h1", text: "You're booked" }),
      h("p", { class: "bk-lead", text: `${booking.typeTitle} with ${booking.page?.name || state.page?.name || ""} is on the calendar.` }),
      state.notice ? h("p", { class: "bk-notice", role: "status", text: state.notice }) : null,
      summary(booking),
      calendarButtons(),
      h("div", { class: "bk-manage-row" },
        h("a", { class: "bk-secondary", href: `?b=${booking.token}` }, "Reschedule or cancel"),
        h("button", { type: "button", class: "bk-secondary", onclick: (event) => copyLink(event.currentTarget) }, icon("copy", "bk-icon bk-icon-sm"), h("span", { "data-label": "", text: "Copy private link" }))));
  }

  function managePanel() {
    const booking = state.booking;
    const cancelled = booking.status === "cancelled";
    const past = !cancelled && booking.endAt < Date.now();
    return h("section", { class: "bk-panel bk-done bk-animate" },
      h("p", { class: `bk-status ${cancelled ? "bk-status-cancelled" : past ? "bk-status-past" : "bk-status-ok"}`, text: cancelled ? "Cancelled" : past ? "This meeting has passed" : "Confirmed" }),
      h("h2", { class: "bk-h1", text: cancelled ? "This meeting is cancelled" : `Your meeting with ${booking.page?.name || ""}` }),
      state.notice ? h("p", { class: "bk-notice", role: "status", text: state.notice }) : null,
      cancelled && booking.cancelReason ? h("blockquote", { class: "bk-quote", text: booking.cancelReason }) : null,
      summary(booking),
      !cancelled && !past ? calendarButtons() : null,
      h("div", { class: "bk-manage-row" },
        booking.canChange && booking.canReschedule ? h("button", { type: "button", class: "bk-primary", onclick: startReschedule }, "Reschedule") : null,
        booking.canChange ? h("button", { type: "button", class: "bk-danger", onclick: () => { state.view = "cancel"; state.notice = ""; render(); } }, "Cancel meeting") : null,
        cancelled && booking.page?.slug ? h("a", { class: "bk-primary", href: `?${booking.page.slug}/${booking.typeId}` }, "Book a new time") : null));
  }

  function cancelPanel() {
    return h("section", { class: "bk-panel bk-animate" },
      h("button", { type: "button", class: "bk-link-back", onclick: () => { state.view = "manage"; render(); } }, icon("left", "bk-icon bk-icon-sm"), "Keep my meeting"),
      h("h2", { class: "bk-h2", text: "Cancel this meeting?" }),
      summary(state.booking),
      h("form", { class: "bk-form", onsubmit: cancelBooking },
        h("label", { class: "bk-field" }, h("span", { class: "bk-label" }, "Reason", h("span", { class: "bk-optional", text: " (optional)" })),
          h("textarea", { name: "reason", rows: 3, maxlength: 600, placeholder: "Let them know why" })),
        h("button", { type: "submit", class: "bk-danger bk-danger-solid", disabled: state.busy }, state.busy ? "Cancelling…" : "Yes, cancel meeting")));
  }

  function startReschedule() {
    const booking = state.booking;
    const page = booking.page || {};
    state.page = page;
    state.type = (page.types || []).find((type) => type.id === booking.typeId) || { id: booking.typeId, title: booking.typeTitle, minutes: booking.minutes, location: booking.location, description: "" };
    state.mode = "reschedule"; state.notice = "";
    void openCalendar();
  }

  function errorPanel(message) {
    return h("section", { class: "bk-panel bk-error-panel bk-animate" },
      h("div", { class: "bk-error-art", icon: "calendar" }),
      h("h1", { class: "bk-h1", text: "Nothing to book here" }),
      h("p", { class: "bk-lead", text: message }));
  }

  function render() {
    const nodes = [];
    root.classList.remove("bk-card-loading");
    root.dataset.view = state.view;
    if (state.view === "missing") {
      root.replaceChildren(errorPanel(state.error || ERRORS.page_not_found));
      return;
    }
    nodes.push(hostPanel());
    const body = state.view === "types" ? typesPanel()
      : state.view === "calendar" ? calendarPanel()
        : state.view === "form" ? formPanel()
          : state.view === "done" ? donePanel()
            : state.view === "manage" ? managePanel()
              : state.view === "cancel" ? cancelPanel() : h("div");
    if (state.error && state.view !== "missing") body.prepend(h("p", { class: "bk-alert", role: "alert", text: state.error }));
    nodes.push(body);
    const focused = document.activeElement?.name;
    root.replaceChildren(...nodes);
    if (focused) root.querySelector(`[name="${focused}"]`)?.focus();
  }

  async function start() {
    const address = parseAddress();
    try {
      if (address.token) {
        state.token = address.token;
        const payload = await api(`/b/${address.token}`);
        state.booking = payload.booking; state.page = payload.booking.page; state.fmt = state.page?.timeFormat || "12h";
        applyAccent(state.page?.accent);
        document.title = `${payload.booking.typeTitle} with ${state.page?.name || ""}`;
        state.view = "manage"; render(); return;
      }
      if (!SLUG_RE.test(address.slug || "")) throw Object.assign(new Error(ERRORS.page_not_found), { code: "page_not_found" });
      state.slug = address.slug;
      const payload = await api(`/p/${address.slug}`);
      state.page = payload.page; state.fmt = payload.page.timeFormat || "12h";
      applyAccent(payload.page.accent);
      document.title = `Book a meeting with ${payload.page.name}`;
      const types = payload.page.types || [];
      const wanted = TYPE_RE.test(address.typeId || "") ? types.find((type) => type.id === address.typeId) : null;
      if (address.typeId && !wanted) state.error = ERRORS.meeting_type_not_found;
      if (wanted || types.length === 1) { state.type = wanted || types[0]; await openCalendar(); return; }
      state.view = "types"; render();
    } catch (error) {
      state.view = "missing"; state.error = error.message; render();
    }
  }

  if (root) void start();
})();

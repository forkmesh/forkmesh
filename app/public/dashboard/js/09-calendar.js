  // ---------------------------------------------------------------------------
  // Calendar page. Private events come from the calendar API; read-only Git
  // activity is fetched directly from each live repository mirror.
  const CALENDAR_COLORS = ["#58a6ff", "#3fb950", "#bc8cff", "#f0883e", "#f778ba", "#39c5cf"];
  const REPOSITORY_COMMITS_CALENDAR = "forkmesh:commits";
  const REPOSITORY_MERGES_CALENDAR = "forkmesh:merges";
  const REPOSITORY_ACTIVITY_CALENDARS = [
    { id: REPOSITORY_COMMITS_CALENDAR, name: "Commits", scope: "repository", role: "reader" },
    { id: REPOSITORY_MERGES_CALENDAR, name: "Merges", scope: "repository", role: "reader" },
  ];
  // Repository activity is fetched per repository per calendar month, so
  // moving between months, switching views or drilling into a day only asks
  // for the months that are not already in hand. The whole-view cache this
  // replaced re-fetched every repository whenever any part of the request
  // changed, which is what made a page showing every commit and merge crawl.
  const CALENDAR_REPOSITORY_CONCURRENCY = 8;
  const CALENDAR_REPOSITORY_CACHE_LIMIT = 480;
  const CALENDAR_AGENDA_PAGE = 200;
  const calendarRepositoryCache = new Map();
  let calendarRepositoryLoadGeneration = 0;
  let calendarRepositoryTargetsCache = { repositories: null, targets: [] };
  const calendarColorCache = new Map();
  let calendarEventsRevision = 0;
  let calendarVisibleCache = { key: "", events: [], buckets: null };
  let calendarAgendaLimit = CALENDAR_AGENDA_PAGE;
  let calendarSearchTimer = 0;
  let calendarRenderTimer = 0;

  // Resolving the system zone builds an Intl formatter, and every projected
  // commit records one. Thousands of commits meant thousands of formatters.
  let calendarSystemZone = "";

  function calendarZone() {
    if (!calendarSystemZone) {
      calendarSystemZone = Intl.DateTimeFormat().resolvedOptions().timeZone || "UTC";
    }
    return calendarSystemZone;
  }

  function calendarValidZone(zone) {
    try {
      new Intl.DateTimeFormat([], { timeZone: zone }).format();
      return zone;
    } catch (_) {
      return calendarZone();
    }
  }

  const calendarZoneFormatterCache = new Map();
  let calendarSystemInvitees = null;
  const calendarOrganizationInvitees = new Map();

  function calendarZoneFormatter(zone) {
    if (!calendarZoneFormatterCache.has(zone)) {
      calendarZoneFormatterCache.set(zone, new Intl.DateTimeFormat("en-CA", {
        timeZone: zone, year: "numeric", month: "2-digit", day: "2-digit",
        hour: "2-digit", minute: "2-digit", second: "2-digit", hourCycle: "h23",
      }));
    }
    return calendarZoneFormatterCache.get(zone);
  }

  function calendarZoneParts(value, zone) {
    const parts = {};
    for (const part of calendarZoneFormatter(zone).formatToParts(value)) {
      if (part.type !== "literal") parts[part.type] = part.value;
    }
    return parts;
  }

  function calendarInputInZone(value, zone) {
    const parts = calendarZoneParts(value, zone);
    return `${parts.year}-${parts.month}-${parts.day}T${parts.hour}:${parts.minute}`;
  }

  function calendarWallTime(value, zone) {
    const match = String(value || "").match(/^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})$/);
    if (!match) return NaN;
    const wall = Date.UTC(Number(match[1]), Number(match[2]) - 1, Number(match[3]), Number(match[4]), Number(match[5]));
    let instant = wall;
    for (let attempt = 0; attempt < 3; attempt += 1) {
      const parts = calendarZoneParts(new Date(instant), zone);
      const represented = Date.UTC(Number(parts.year), Number(parts.month) - 1, Number(parts.day), Number(parts.hour), Number(parts.minute), Number(parts.second));
      const corrected = instant + wall - represented;
      if (corrected === instant) break;
      instant = corrected;
    }
    return instant;
  }

  // Labelling a zone builds an Intl formatter, and the picker holds every zone
  // the platform knows. Building all of them on each dialog open cost more
  // than the dialog itself, and a zone's short offset only moves at a DST
  // boundary, so hold both the labels and the built option markup.
  const calendarZoneLabelCache = new Map();

  function calendarZoneOffsetLabel(zone, value = new Date()) {
    const cached = calendarZoneLabelCache.get(zone);
    if (cached) return cached;
    let label = zone;
    try {
      const name = new Intl.DateTimeFormat([], { timeZone: zone, timeZoneName: "shortOffset" })
        .formatToParts(value).find((part) => part.type === "timeZoneName")?.value || "UTC";
      label = `${zone.replaceAll("_", " ")} (${name})`;
    } catch (_) {
      label = zone;
    }
    calendarZoneLabelCache.set(zone, label);
    return label;
  }

  function populateCalendarTimezones(selected = calendarZone()) {
    const select = $("[data-calendar-timezone]");
    if (!select) return;
    selected = calendarValidZone(selected);
    let zones = [];
    try {
      zones = typeof Intl.supportedValuesOf === "function" ? Intl.supportedValuesOf("timeZone") : [];
    } catch (_) {
      zones = [];
    }
    zones = [...new Set([selected, calendarZone(), "UTC", ...zones].filter(Boolean))];
    const signature = zones.length ? `${zones.length}:${zones[0]}` : "0:";
    if (select.dataset.zonesBuilt !== signature) {
      select.innerHTML = zones.map((zone) => `<option value="${escapeHtml(zone)}">${escapeHtml(calendarZoneOffsetLabel(zone))}</option>`).join("");
      select.dataset.zonesBuilt = signature;
    }
    select.value = selected;
    select.dataset.previousZone = selected;
    const hint = $("[data-calendar-timezone-hint]");
    if (hint) hint.textContent = `Times will be saved in ${calendarZoneOffsetLabel(selected)}.`;
  }

  function calendarDayStart(value) {
    const date = new Date(value);
    date.setHours(0, 0, 0, 0);
    return date;
  }

  function calendarWeekStart(value) {
    const date = calendarDayStart(value);
    date.setDate(date.getDate() - ((date.getDay() + 6) % 7));
    return date;
  }

  function calendarAddDays(value, days) {
    const date = new Date(value);
    date.setDate(date.getDate() + days);
    return date;
  }

  function calendarDateKey(value) {
    const date = new Date(value);
    return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, "0")}-${String(date.getDate()).padStart(2, "0")}`;
  }

  function calendarLocalInput(value) {
    const date = new Date(value);
    const offset = date.getTimezoneOffset() * 60000;
    return new Date(date.getTime() - offset).toISOString().slice(0, 16);
  }

  function calendarColor(id) {
    const key = String(id || "personal");
    const cached = calendarColorCache.get(key);
    if (cached) return cached;
    let hash = 0;
    for (const character of key) hash = ((hash << 5) - hash) + character.charCodeAt(0);
    const color = CALENDAR_COLORS[Math.abs(hash) % CALENDAR_COLORS.length];
    calendarColorCache.set(key, color);
    return color;
  }

  function calendarVisibleRange() {
    const anchor = state.calendarView.anchor;
    if (state.calendarView.mode === "day") {
      const start = calendarDayStart(anchor);
      return [start, calendarAddDays(start, 1)];
    }
    if (state.calendarView.mode === "week") {
      const start = calendarWeekStart(anchor);
      return [start, calendarAddDays(start, 7)];
    }
    if (state.calendarView.mode === "agenda") {
      const start = calendarDayStart(anchor);
      return [start, calendarAddDays(start, 90)];
    }
    const first = new Date(anchor.getFullYear(), anchor.getMonth(), 1);
    const start = calendarWeekStart(first);
    return [start, calendarAddDays(start, 42)];
  }

  // An occurrence is a light wrapper over its source event rather than a copy
  // of it. Repository activity runs to thousands of commits in a window, and
  // spreading every field of every one of them on every render was the bulk of
  // the render cost. `dayKey` is derived once here so the day bucketing below
  // never builds a Date per event per calendar cell.
  const CALENDAR_RECURRENCES = new Set(["daily", "weekly", "monthly", "yearly"]);
  const CALENDAR_NO_EVENTS = Object.freeze([]);

  function calendarOccurrence(event, startAt, endAt) {
    const date = new Date(startAt);
    return {
      event,
      id: event.id,
      calendar: event.calendar,
      title: event.title,
      allDay: event.allDay,
      occurrenceStart: startAt,
      occurrenceEnd: endAt,
      dayKey: `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, "0")}-${String(date.getDate()).padStart(2, "0")}`,
    };
  }

  function calendarOccurrences(event, start, end, output) {
    const first = Number(event.startAt || 0);
    const duration = Math.max(1, Number(event.endAt || 0) - first);
    const recurrence = event.recurrence || "none";
    const startMs = start.getTime();
    const endMs = end.getTime();
    const until = Number(event.recurrenceUntil || 0) || endMs;
    if (!CALENDAR_RECURRENCES.has(recurrence)) {
      if (first < endMs && first <= until && first + duration > startMs) {
        output.push(calendarOccurrence(event, first, first + duration));
      }
      return output;
    }
    const current = new Date(first);
    for (let guard = 0; guard < 500 && current.getTime() < endMs && current.getTime() <= until; guard += 1) {
      const at = current.getTime();
      if (at + duration > startMs) output.push(calendarOccurrence(event, at, at + duration));
      if (recurrence === "daily") current.setDate(current.getDate() + 1);
      else if (recurrence === "weekly") current.setDate(current.getDate() + 7);
      else if (recurrence === "monthly") current.setMonth(current.getMonth() + 1);
      else current.setFullYear(current.getFullYear() + 1);
    }
    return output;
  }

  // Searching lower-cases six fields per event. Doing that per keystroke over
  // every commit in the window dominated typing, so each event keeps the
  // folded text it was built from.
  function calendarSearchText(event) {
    if (event._searchText === undefined) {
      event._searchText = [event.title, event.description, event.location, event.owner, event.repository, event.hash]
        .map((value) => String(value || "").toLowerCase()).join("\u001f");
    }
    return event._searchText;
  }

  // Bump whenever either event list is replaced, so the memo below knows the
  // difference between a re-render and new data.
  function calendarEventsChanged() {
    calendarEventsRevision += 1;
  }

  function visibleCalendarEvents() {
    const [start, end] = calendarVisibleRange();
    const query = state.calendarView.query.trim().toLowerCase();
    const selected = state.calendarView.selected;
    const key = [start.getTime(), end.getTime(), calendarEventsRevision, query,
      [...selected].sort().join(",")].join("|");
    if (calendarVisibleCache.key === key) return calendarVisibleCache.events;
    const output = [];
    for (const source of [state.calendarView.events, state.calendarView.repositoryEvents]) {
      for (const event of source) {
        if (!selected.has(event.calendar || "personal")) continue;
        if (query && !calendarSearchText(event).includes(query)) continue;
        calendarOccurrences(event, start, end, output);
      }
    }
    output.sort((a, b) => a.occurrenceStart - b.occurrenceStart);
    calendarVisibleCache = { key, events: output, buckets: null };
    calendarAgendaLimit = CALENDAR_AGENDA_PAGE;
    return output;
  }

  // Month and week used to re-scan the whole event list once per rendered day.
  // One pass into day buckets turns that back into a lookup.
  function calendarDayBuckets(events) {
    if (calendarVisibleCache.events === events && calendarVisibleCache.buckets) {
      return calendarVisibleCache.buckets;
    }
    const buckets = new Map();
    for (const item of events) {
      const existing = buckets.get(item.dayKey);
      if (existing) existing.push(item);
      else buckets.set(item.dayKey, [item]);
    }
    if (calendarVisibleCache.events === events) calendarVisibleCache.buckets = buckets;
    return buckets;
  }

  function calendarEventButton(event, compact = false) {
    const start = new Date(event.occurrenceStart);
    const time = event.allDay ? "All day" : start.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
    return `<button type="button" data-calendar-event="${escapeHtml(event.id)}" class="fm-cal-event" style="--event-color:${calendarColor(event.calendar)}" title="${escapeHtml(`${time} · ${event.title}`)}">${compact ? "" : `<span class="font-mono opacity-70">${escapeHtml(time)} </span>`}${escapeHtml(event.title)}</button>`;
  }

  function renderCalendarList() {
    const list = $("[data-calendar-list]");
    const scope = $("[data-calendar-scope]");
    if (!list) return;
    const eventCalendars = state.calendarView.calendars.some((calendar) => calendar.id === "personal")
      ? state.calendarView.calendars
      : [{ id: "personal", name: "My calendar", scope: "personal", role: "owner" }, ...state.calendarView.calendars];
    const calendars = [...eventCalendars, ...REPOSITORY_ACTIVITY_CALENDARS];
    list.innerHTML = calendars.map((calendar, index) => `
      <label class="flex items-center gap-2 rounded-md px-2 py-1.5 text-foreground hover:bg-secondary">
        <input data-calendar-toggle="${escapeHtml(calendar.id)}" type="checkbox" ${state.calendarView.selected.has(calendar.id) ? "checked" : ""}>
        <span class="h-2.5 w-2.5 rounded-full" style="background:${CALENDAR_COLORS[index % CALENDAR_COLORS.length]}"></span>
        <span class="min-w-0 flex-1 truncate">${escapeHtml(calendar.name)}</span>
        ${calendar.scope === "repository" ? `<i data-lucide="${calendar.id === REPOSITORY_MERGES_CALENDAR ? "git-merge" : "git-commit-horizontal"}" class="h-3 w-3 text-muted-foreground"></i>` : calendar.scope === "organization" ? '<i data-lucide="users" class="h-3 w-3 text-muted-foreground"></i>' : '<i data-lucide="lock" class="h-3 w-3 text-muted-foreground"></i>'}
      </label>`).join("");
    if (scope) {
      scope.innerHTML = eventCalendars.map((calendar) =>
        `<option value="${escapeHtml(calendar.id)}">${escapeHtml(calendar.name)}${calendar.scope === "organization" ? " · team" : " · private"}</option>`).join("");
    }
    window.lucide?.createIcons();
  }

  function renderCalendarMonth(content, events) {
    const [start] = calendarVisibleRange();
    const buckets = calendarDayBuckets(events);
    const names = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];
    const headers = names.map((name) => `<div class="border-b border-r border-border bg-card px-2 py-1.5 text-center text-[11px] font-semibold text-muted-foreground">${name}</div>`).join("");
    let cells = "";
    const today = calendarDateKey(new Date());
    for (let offset = 0; offset < 42; offset += 1) {
      const date = calendarAddDays(start, offset);
      const key = calendarDateKey(date);
      const daily = buckets.get(key) || CALENDAR_NO_EVENTS;
      const muted = date.getMonth() !== state.calendarView.anchor.getMonth();
      cells += `<div class="fm-cal-cell ${muted ? "opacity-45" : ""}" data-calendar-day="${key}">
        <button type="button" data-calendar-open-day="${key}" ${key === today ? 'aria-current="date" ' : ""}class="${key === today ? "bg-accent text-accent-foreground" : "text-muted-foreground"} h-6 min-w-6 rounded-full px-1 text-xs font-semibold">${date.getDate()}</button>
        ${daily.slice(0, 4).map((event) => calendarEventButton(event, true)).join("")}
        ${daily.length > 4 ? `<button data-calendar-open-day="${key}" class="mt-1 text-[10px] text-accent">+${daily.length - 4} more</button>` : ""}
      </div>`;
    }
    content.innerHTML = `<div class="fm-cal-grid" style="grid-template-rows:auto repeat(6,minmax(7.5rem,1fr))">${headers}${cells}</div>`;
  }

  // A single day of repository activity can run to hundreds of commits, so a
  // week column shows a readable slice and defers the rest to the day view -
  // the same contract the month cells already keep.
  const CALENDAR_WEEK_DAY_LIMIT = 24;

  function renderCalendarWeek(content, events) {
    const [start] = calendarVisibleRange();
    const buckets = calendarDayBuckets(events);
    const today = calendarDateKey(new Date());
    content.innerHTML = `<div class="fm-cal-week p-3">${Array.from({ length: 7 }, (_, index) => {
      const date = calendarAddDays(start, index);
      const key = calendarDateKey(date);
      const daily = buckets.get(key) || CALENDAR_NO_EVENTS;
      const shown = daily.length > CALENDAR_WEEK_DAY_LIMIT ? daily.slice(0, CALENDAR_WEEK_DAY_LIMIT) : daily;
      const overflow = daily.length - shown.length;
      return `<section class="fm-cal-day ${key === today ? "fm-cal-today" : ""}"><button data-calendar-open-day="${key}" ${key === today ? 'aria-current="date" ' : ""}class="mb-3 block w-full border-b border-border pb-2 text-left"><span class="block text-[10px] font-semibold uppercase ${key === today ? "text-accent" : "text-muted-foreground"}">${date.toLocaleDateString([], { weekday: "short" })}</span><span class="${key === today ? "inline-block rounded-full bg-accent px-2 text-accent-foreground" : "text-foreground"} text-lg font-semibold">${date.getDate()}</span></button>${daily.length ? shown.map((event) => calendarEventButton(event)).join("") : '<p class="py-4 text-center text-xs text-muted-foreground">No events</p>'}${overflow > 0 ? `<button data-calendar-open-day="${key}" class="mt-1 text-[10px] text-accent">+${formatCount(overflow)} more</button>` : ""}</section>`;
    }).join("")}</div>`;
  }

  function calendarDetailRow(item) {
    const event = item.event;
    const start = new Date(item.occurrenceStart);
    const end = new Date(item.occurrenceEnd);
    const invited = (event.attendees || []).map((person) => `${person.name} (${person.status})`).join(", ");
    const secondary = event.readOnly
      ? [event.repository, event.owner, String(event.hash || "").slice(0, 12)].filter(Boolean)
      : [event.location, invited, event.calendar === "personal" ? "Private" : event.calendar].filter(Boolean);
    return `<article class="flex gap-3 border-b border-border px-4 py-3 hover:bg-secondary/40">
      <div class="w-20 shrink-0 text-xs font-mono text-muted-foreground">${item.allDay ? "All day" : `${start.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" })}<br>${end.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" })}`}</div>
      <button data-calendar-event="${escapeHtml(item.id)}" type="button" class="min-w-0 flex-1 text-left"><span class="block font-medium text-foreground">${escapeHtml(item.title)}</span><span class="mt-1 block text-xs text-muted-foreground">${escapeHtml(secondary.join(" · "))}</span></button>
      <span class="mt-1 h-3 w-3 shrink-0 rounded-full" style="background:${calendarColor(item.calendar)}"></span>
    </article>`;
  }

  // The agenda spans 90 days, so with every commit and merge on it the full
  // list is tens of thousands of rows. Render a page at a time and let the
  // reader ask for the rest rather than building all of that DOM up front.
  function renderCalendarAgenda(content, events) {
    const shown = events.length > calendarAgendaLimit
      ? events.slice(0, calendarAgendaLimit)
      : events;
    const groups = new Map();
    for (const item of shown) {
      const existing = groups.get(item.dayKey);
      if (existing) existing.push(item);
      else groups.set(item.dayKey, [item]);
    }
    const remaining = events.length - shown.length;
    const more = remaining > 0
      ? `<button type="button" data-calendar-agenda-more class="w-full border-t border-border px-4 py-3 text-center text-xs font-medium text-accent hover:bg-secondary/40">Show ${formatCount(Math.min(remaining, CALENDAR_AGENDA_PAGE))} more · ${formatCount(remaining)} remaining</button>`
      : "";
    content.innerHTML = groups.size ? Array.from(groups).map(([key, items]) => {
      const date = new Date(`${key}T12:00:00`);
      return `<section><h3 class="sticky top-[37px] z-[5] border-b border-border bg-card px-4 py-2 text-xs font-semibold text-foreground">${escapeHtml(date.toLocaleDateString([], { weekday: "long", month: "long", day: "numeric" }))}</h3>${items.map(calendarDetailRow).join("")}</section>`;
    }).join("") + more : '<div class="fm-blankslate"><i data-lucide="calendar" aria-hidden="true"></i><p class="fm-blankslate-title">No events</p><p class="fm-blankslate-desc">No events in this period.</p></div>';
  }

  function renderCalendar() {
    const content = $("[data-calendar-content]");
    const heading = $("[data-calendar-heading]");
    const status = $("[data-calendar-status]");
    if (!content) return;
    for (const button of $$('[data-calendar-mode]')) {
      const active = button.dataset.calendarMode === state.calendarView.mode;
      button.classList.toggle("bg-secondary", active);
      button.classList.toggle("text-foreground", active);
    }
    if (state.calendarView.loading ||
        (state.calendarView.error && !state.calendarView.repositoryLoading &&
         !state.calendarView.repositoryEvents.length)) {
      content.innerHTML = state.calendarView.loading
        ? `<div class="p-10 text-sm text-muted-foreground">${loadingHtml("Loading your calendars...")}</div>`
        : `<div class="fm-blankslate"><i data-lucide="circle-alert" aria-hidden="true"></i><p class="fm-blankslate-title">Your calendars did not load</p><p class="fm-blankslate-desc">${escapeHtml(state.calendarView.error)}</p></div>`;
      if (status) status.textContent = "";
      window.lucide?.createIcons();
      return;
    }
    const anchor = state.calendarView.anchor;
    if (heading) heading.textContent = state.calendarView.mode === "day"
      ? anchor.toLocaleDateString([], { weekday: "long", month: "long", day: "numeric", year: "numeric" })
      : state.calendarView.mode === "week"
        ? `Week of ${calendarWeekStart(anchor).toLocaleDateString([], { month: "long", day: "numeric", year: "numeric" })}`
        : state.calendarView.mode === "agenda"
          ? `Agenda from ${anchor.toLocaleDateString([], { month: "long", day: "numeric" })}`
          : anchor.toLocaleDateString([], { month: "long", year: "numeric" });
    const events = visibleCalendarEvents();
    if (status) {
      const privateState = state.calendarView.error
        ? ` · ${state.calendarView.error}` : "";
      const repositoryState = state.calendarView.repositoryLoading
        ? " · loading repository activity"
        : state.calendarView.repositoryError ? ` · ${state.calendarView.repositoryError}` : "";
      status.textContent = `${events.length} event${events.length === 1 ? "" : "s"}${privateState}${repositoryState}`;
    }
    if (state.calendarView.mode === "month") renderCalendarMonth(content, events);
    else if (state.calendarView.mode === "week") renderCalendarWeek(content, events);
    else renderCalendarAgenda(content, events);
    // No view emits a data-lucide node, so the document-wide icon sweep this
    // used to run after every render (including every keystroke) was pure cost.
  }

  // Repository activity streams in per repository, so coalesce the renders it
  // triggers instead of rebuilding the grid once per response.
  function scheduleCalendarRender() {
    if (calendarRenderTimer) return;
    calendarRenderTimer = setTimeout(() => {
      calendarRenderTimer = 0;
      renderCalendar();
    }, 60);
  }

  async function calendarRequest(method, path, body) {
    const token = state.session?.sessionToken || "";
    const response = await fetch(path, {
      method,
      cache: "no-store",
      headers: { accept: "application/json", "content-type": "application/json", ...(token ? { authorization: `Bearer ${token}` } : {}) },
      body: method === "GET" ? undefined : JSON.stringify(body || {}),
    });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(String(payload.error || `HTTP ${response.status}`).replaceAll("_", " "));
      error.status = response.status;
      throw error;
    }
    return payload;
  }

  function repositoryCalendarEvent(repository, commit) {
    const hash = String(commit?.hash || "").trim().toLowerCase();
    const committedAt = Date.parse(String(commit?.date || ""));
    if (!/^(?:[0-9a-f]{40}|[0-9a-f]{64})$/.test(hash) || !Number.isFinite(committedAt)) return null;
    const parents = String(commit?.parents || "").trim();
    const merge = parents.split(/\s+/).filter(Boolean).length > 1;
    const author = String(commit?.author || "").trim();
    const subject = String(commit?.subject || "").trim() || `Commit ${hash.slice(0, 12)}`;
    return {
      id: `git:${repository}:${hash}`,
      source: "repository", kind: merge ? "merge" : "commit", readOnly: true,
      calendar: merge ? REPOSITORY_MERGES_CALENDAR : REPOSITORY_COMMITS_CALENDAR,
      scope: "repository", repository, hash, parents, owner: author,
      startAt: committedAt, endAt: committedAt + 60000, allDay: false,
      timezone: calendarZone(), title: subject, location: repository,
      description: `${merge ? "Merge" : "Commit"} ${hash}\n${author} · ${repository}`,
      reminders: [], recurrence: "none", recurrenceUntil: 0, attendees: [],
    };
  }

  // Grouping every catalog row is not free and the answer only changes when
  // the catalog does, so hold it against the repository list it was built from.
  function calendarRepositoryTargets() {
    const repositories = state.repositories || [];
    if (calendarRepositoryTargetsCache.repositories === repositories) {
      return calendarRepositoryTargetsCache.targets;
    }
    const targets = mergeGroupsByPublicAddress(groupRepositories(repositories))
      .map((group) => {
        const repo = sourceOfTruth(group);
        const owner = groupDisplayOwner(group) || repo?.owner || "";
        const name = String(repo?.name || "").trim();
        return repo && name ? { repo, repository: `${owner || "owner"}/${name}` } : null;
      })
      .filter((target) => target && repoIsLive(target.repo));
    calendarRepositoryTargetsCache = { repositories, targets };
    return targets;
  }

  // Whole calendar months are the unit both the cache and the mirror read work
  // in. A month view spans at most three of them, so paging to the next month
  // asks each repository for one month it does not already hold, and switching
  // view or opening a day asks for nothing at all.
  function calendarMonthWindows(start, end) {
    const windows = [];
    const cursor = new Date(start.getFullYear(), start.getMonth(), 1);
    while (cursor.getTime() < end.getTime() && windows.length < 24) {
      const next = new Date(cursor.getFullYear(), cursor.getMonth() + 1, 1);
      windows.push({ start: cursor.getTime(), end: next.getTime() });
      cursor.setMonth(cursor.getMonth() + 1);
    }
    return windows;
  }

  function rememberCalendarRepositoryMonth(key, events) {
    calendarRepositoryCache.set(key, events);
    while (calendarRepositoryCache.size > CALENDAR_REPOSITORY_CACHE_LIMIT) {
      calendarRepositoryCache.delete(calendarRepositoryCache.keys().next().value);
    }
  }

  async function loadCalendarRepositoryEvents() {
    const [start, end] = calendarVisibleRange();
    const targets = calendarRepositoryTargets();
    const months = calendarMonthWindows(start, end);
    const jobs = [];
    for (const target of targets) {
      const version = repoDataVersion(target.repo);
      for (const month of months) {
        jobs.push({ target, month, key: `${target.repository}@${version}|${month.start}` });
      }
    }
    const requestKey = jobs.map((job) => job.key).join("|");
    if (state.calendarView.repositoryRange === requestKey) return;

    const generation = ++calendarRepositoryLoadGeneration;
    state.calendarView.repositoryRange = requestKey;
    const collected = new Map();
    const failed = new Set();
    const pending = [];
    for (const job of jobs) {
      const cached = calendarRepositoryCache.get(job.key);
      if (cached) for (const event of cached) collected.set(event.id, event);
      else pending.push(job);
    }
    // Publish whatever the cache already answers before any request goes out,
    // so navigation inside a month the page has seen paints immediately.
    const publish = (loading) => {
      state.calendarView.repositoryEvents = [...collected.values()];
      state.calendarView.repositoryError = failed.size
        ? `${failed.size} repo${failed.size === 1 ? "" : "s"} unavailable` : "";
      state.calendarView.repositoryLoading = loading;
      calendarEventsChanged();
    };
    publish(pending.length > 0);
    renderCalendar();
    if (!pending.length) return;

    let cursor = 0;
    const loadNext = async () => {
      while (cursor < pending.length) {
        const job = pending[cursor++];
        let events = [];
        try {
          const data = await fetchRepoJson(repoLiveUrl(job.target.repo, "history", {
            ref: "", all: "1",
            since: Math.floor(job.month.start / 1000),
            until: Math.floor((job.month.end - 1) / 1000),
          }));
          for (const commit of Array.isArray(data.commits) ? data.commits : []) {
            const event = repositoryCalendarEvent(job.target.repository, commit);
            if (event && event.startAt >= job.month.start && event.startAt < job.month.end) {
              events.push(event);
            }
          }
          rememberCalendarRepositoryMonth(job.key, events);
        } catch (_) {
          events = [];
          failed.add(job.target.repository);
        }
        if (generation !== calendarRepositoryLoadGeneration) return;
        for (const event of events) collected.set(event.id, event);
        publish(cursor < pending.length);
        scheduleCalendarRender();
      }
    };
    await Promise.all(Array.from(
      { length: Math.min(CALENDAR_REPOSITORY_CONCURRENCY, pending.length) }, loadNext));
    if (generation !== calendarRepositoryLoadGeneration) return;
    publish(false);
    renderCalendar();
  }

  async function loadCalendarEvents() {
    if (!state.session) {
      state.calendarView.loading = false;
      state.calendarView.error = "Sign in to open your private calendars.";
      renderCalendar();
      return;
    }
    state.calendarView.loading = true;
    state.calendarView.error = "";
    renderCalendar();
    try {
      const start = new Date(); start.setFullYear(start.getFullYear() - 1);
      const end = new Date(); end.setFullYear(end.getFullYear() + 2);
      const payload = await calendarRequest("GET", `/api/calendar?start=${start.getTime()}&end=${end.getTime()}`);
      state.calendarView.events = Array.isArray(payload.events) ? payload.events : [];
      state.calendarView.calendars = Array.isArray(payload.calendars) ? payload.calendars : [];
      state.calendarView.actor = payload.actor || "";
      calendarEventsChanged();
      calendarOrganizationInvitees.clear();
      for (const calendar of state.calendarView.calendars) {
        state.calendarView.selected.add(calendar.id);
        if (calendar.scope === "organization") {
          calendarOrganizationInvitees.set(calendar.id, (calendar.members || [])
            .map((name) => String(name || "").trim().toLowerCase()).filter(Boolean));
        }
      }
      renderCalendarList();
    } catch (error) {
      state.calendarView.error = `Calendar unavailable: ${error.message || "request failed"}`;
    } finally {
      state.calendarView.loading = false;
      renderCalendar();
    }
  }

  async function loadCalendarInvitees(calendarId = "personal") {
    const organization = calendarId === "personal" ? "" : calendarId;
    const requests = [];
    if (!calendarSystemInvitees) {
      requests.push(calendarRequest("GET", "/api/accounts/users").then((payload) => {
        calendarSystemInvitees = (payload.users || []).map((user) => String(user.name || "").trim().toLowerCase()).filter(Boolean);
      }).catch(() => { calendarSystemInvitees = []; }));
    }
    await Promise.all(requests);
    if (($("[data-calendar-scope]")?.value || "personal") !== calendarId) return;
    const members = new Set(calendarOrganizationInvitees.get(organization) || []);
    const names = [...new Set([...(members || []), ...(calendarSystemInvitees || [])])]
      .filter((name) => name && name !== state.calendarView.actor)
      .sort((left, right) => Number(members.has(right)) - Number(members.has(left)) || left.localeCompare(right));
    state.calendarView.invitees = names.map((name) => ({ name, organization: members.has(name) ? organization : "" }));
    renderCalendarInviteeSuggestions();
  }

  function calendarInviteeToken(input) {
    return String(input?.value || "").split(",").pop().trim().toLowerCase();
  }

  function renderCalendarInviteeSuggestions() {
    const input = $("[data-calendar-attendees]");
    const list = $("[data-calendar-attendee-suggestions]");
    if (!input || !list) return;
    const token = calendarInviteeToken(input);
    const chosen = new Set(String(input.value || "").split(",").slice(0, -1).map((name) => name.trim().toLowerCase()).filter(Boolean));
    const matches = (state.calendarView.invitees || [])
      .filter((candidate) => !chosen.has(candidate.name) && (!token || candidate.name.includes(token)))
      .slice(0, 10);
    if (!matches.length || document.activeElement !== input) {
      list.hidden = true;
      input.setAttribute("aria-expanded", "false");
      return;
    }
    list.innerHTML = matches.map((candidate, index) => `<button type="button" role="option" data-calendar-attendee-option="${escapeHtml(candidate.name)}" data-calendar-attendee-index="${index}" class="fm-menu-item focus:bg-secondary"><span class="min-w-0 truncate">@${escapeHtml(candidate.name)}</span><span class="fm-menu-hint">${candidate.organization ? escapeHtml(candidate.organization) + " member" : `${escapeHtml(platformName())} user`}</span></button>`).join("");
    list.hidden = false;
    input.setAttribute("aria-expanded", "true");
  }

  function chooseCalendarInvitee(name) {
    const input = $("[data-calendar-attendees]");
    if (!input) return;
    const values = String(input.value || "").split(",");
    values[values.length - 1] = ` ${name}`;
    input.value = values.map((value) => value.trim()).filter(Boolean).join(", ") + ", ";
    input.focus();
    renderCalendarInviteeSuggestions();
  }

  function calendarEventById(id) {
    for (const source of [state.calendarView.events, state.calendarView.repositoryEvents]) {
      for (const event of source) if (event.id === id) return event;
    }
    return null;
  }

  function openCalendarDialog(event = null, date = null) {
    const modal = $("[data-calendar-modal]");
    if (!modal) return;
    const start = event ? new Date(event.startAt) : date ? new Date(`${date}T09:00:00`) : new Date(Date.now() + 3600000);
    if (!event) start.setMinutes(Math.ceil(start.getMinutes() / 15) * 15, 0, 0);
    const end = event ? new Date(event.endAt) : new Date(start.getTime() + 3600000);
    state.calendarView.editingId = event?.id || "";
    const readOnly = Boolean(event?.readOnly);
    const calendarRole = state.calendarView.calendars.find((item) => item.id === event?.calendar)?.role || "";
    const canManage = !readOnly && (!event || event.owner === state.calendarView.actor || ["owner", "admin"].includes(calendarRole));
    $("[data-calendar-dialog-title]").textContent = readOnly ? `${event.kind === "merge" ? "Merge" : "Commit"} details` : event ? "Edit event" : "New event";
    $("[data-calendar-title]").value = event?.title || "";
    const timezone = calendarValidZone(event?.timezone || calendarZone());
    populateCalendarTimezones(timezone);
    $("[data-calendar-start]").value = calendarInputInZone(start, timezone);
    $("[data-calendar-end]").value = calendarInputInZone(end, timezone);
    $("[data-calendar-all-day]").checked = Boolean(event?.allDay);
    const calendarSelect = $("[data-calendar-scope]");
    calendarSelect.querySelectorAll("[data-repository-activity]").forEach((option) => option.remove());
    if (readOnly) {
      calendarSelect.insertAdjacentHTML("beforeend", `<option data-repository-activity value="${escapeHtml(event.calendar)}">${event.kind === "merge" ? "Merges" : "Commits"} · read-only</option>`);
    }
    calendarSelect.value = event?.calendar || "personal";
    $("[data-calendar-recurrence]").value = event?.recurrence || "none";
    $("[data-calendar-location]").value = event?.location || "";
    $("[data-calendar-attendees]").value = (event?.attendees || []).map((person) => person.name).join(", ");
    $("[data-calendar-meeting]").value = event?.meetingUrl || "";
    $("[data-calendar-description]").value = event?.description || "";
    for (const input of $$('[data-calendar-reminder]')) input.checked = (event?.reminders || [1440, 60, 10]).includes(Number(input.value));
    $("[data-calendar-delete]").hidden = !event || !canManage;
    $("[data-calendar-save]").hidden = !canManage;
    for (const input of $$('[data-calendar-form] input, [data-calendar-form] select, [data-calendar-form] textarea')) {
      input.disabled = Boolean(event && !canManage);
    }
    const invitation = readOnly ? null : event?.attendees?.find((person) => person.name === state.calendarView.actor);
    $("[data-calendar-rsvp]").hidden = !invitation;
    $("[data-calendar-form-status]").textContent = readOnly
      ? `Read-only repository activity · ${String(event.hash || "").slice(0, 12)}`
      : invitation ? `Invitation: ${invitation.status}` : "";
    modal.hidden = false;
    $("[data-calendar-title]").focus();
    if (!readOnly) void loadCalendarInvitees(calendarSelect.value || "personal");
    window.lucide?.createIcons();
  }

  function closeCalendarDialog() {
    const modal = $("[data-calendar-modal]");
    if (modal) modal.hidden = true;
    $("[data-calendar-save]").hidden = false;
    state.calendarView.editingId = "";
  }

  function calendarFormPayload() {
    const calendar = $("[data-calendar-scope]").value || "personal";
    const timezone = $("[data-calendar-timezone]").value || calendarZone();
    return {
      title: $("[data-calendar-title]").value.trim(),
      startAt: calendarWallTime($("[data-calendar-start]").value, timezone),
      endAt: calendarWallTime($("[data-calendar-end]").value, timezone),
      allDay: $("[data-calendar-all-day]").checked,
      scope: calendar === "personal" ? "personal" : "organization",
      calendar,
      timezone,
      recurrence: $("[data-calendar-recurrence]").value,
      location: $("[data-calendar-location]").value.trim(),
      attendees: $("[data-calendar-attendees]").value.split(",").map((name) => name.trim().toLowerCase()).filter(Boolean),
      meetingUrl: $("[data-calendar-meeting]").value.trim(),
      description: $("[data-calendar-description]").value,
      reminders: $$('[data-calendar-reminder]:checked').map((input) => Number(input.value)),
    };
  }

  async function saveCalendarEvent() {
    const status = $("[data-calendar-form-status]");
    const button = $("[data-calendar-save]");
    const payload = calendarFormPayload();
    if (!payload.title || !Number.isFinite(payload.startAt) || payload.endAt <= payload.startAt) {
      status.textContent = "Add a title and a valid end time.";
      return;
    }
    button.disabled = true;
    status.textContent = "Saving...";
    try {
      const editing = state.calendarView.editingId;
      await calendarRequest(editing ? "PATCH" : "POST", editing ? `/api/calendar/${editing}` : "/api/calendar", payload);
      closeCalendarDialog();
      await loadCalendarEvents();
    } catch (error) {
      status.textContent = `Could not save: ${error.message}`;
    } finally {
      button.disabled = false;
    }
  }

  async function deleteCalendarEvent() {
    const id = state.calendarView.editingId;
    if (!id || !confirm("Delete this event for everyone who can see it?")) return;
    try {
      await calendarRequest("DELETE", `/api/calendar/${id}`, {});
      closeCalendarDialog();
      await loadCalendarEvents();
    } catch (error) {
      $("[data-calendar-form-status]").textContent = `Could not delete: ${error.message}`;
    }
  }

  async function calendarRsvp(status) {
    const id = state.calendarView.editingId;
    if (!id) return;
    try {
      await calendarRequest("POST", `/api/calendar/${id}/rsvp`, { status });
      $("[data-calendar-form-status]").textContent = `Response saved: ${status}`;
      await loadCalendarEvents();
    } catch (error) {
      $("[data-calendar-form-status]").textContent = `Could not respond: ${error.message}`;
    }
  }

  function moveCalendarAnchor(direction) {
    const anchor = new Date(state.calendarView.anchor);
    if (state.calendarView.mode === "month") anchor.setMonth(anchor.getMonth() + direction);
    else if (state.calendarView.mode === "week") anchor.setDate(anchor.getDate() + 7 * direction);
    else if (state.calendarView.mode === "agenda") anchor.setDate(anchor.getDate() + 30 * direction);
    else anchor.setDate(anchor.getDate() + direction);
    state.calendarView.anchor = anchor;
    renderCalendar();
    void loadCalendarRepositoryEvents();
  }

  function initCalendarPage() {
    const zone = $("[data-calendar-zone]");
    if (zone) zone.textContent = calendarZone();
    $("[data-calendar-new]")?.addEventListener("click", () => openCalendarDialog());
    $("[data-calendar-today]")?.addEventListener("click", () => { state.calendarView.anchor = new Date(); renderCalendar(); void loadCalendarRepositoryEvents(); });
    $("[data-calendar-prev]")?.addEventListener("click", () => moveCalendarAnchor(-1));
    $("[data-calendar-next]")?.addEventListener("click", () => moveCalendarAnchor(1));
    $("[data-calendar-modes]")?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-calendar-mode]");
      if (!button) return;
      state.calendarView.mode = button.dataset.calendarMode;
      renderCalendar();
      void loadCalendarRepositoryEvents();
    });
    $("[data-calendar-list]")?.addEventListener("change", (event) => {
      const input = event.target.closest("[data-calendar-toggle]");
      if (!input) return;
      if (input.checked) state.calendarView.selected.add(input.dataset.calendarToggle);
      else state.calendarView.selected.delete(input.dataset.calendarToggle);
      renderCalendar();
    });
    // Every keystroke re-filters every commit in the window, so let the typist
    // finish the word before re-running it.
    $("[data-calendar-search]")?.addEventListener("input", (event) => {
      const value = event.target.value;
      clearTimeout(calendarSearchTimer);
      calendarSearchTimer = setTimeout(() => {
        state.calendarView.query = value;
        renderCalendar();
      }, 180);
    });
    $("[data-calendar-scope]")?.addEventListener("change", (event) => { void loadCalendarInvitees(event.target.value || "personal"); });
    $("[data-calendar-timezone]")?.addEventListener("change", (event) => {
      const select = event.target;
      const previous = select.dataset.previousZone || calendarZone();
      const next = select.value || calendarZone();
      for (const selector of ["[data-calendar-start]", "[data-calendar-end]"]) {
        const field = $(selector);
        const instant = calendarWallTime(field.value, previous);
        if (Number.isFinite(instant)) field.value = calendarInputInZone(new Date(instant), next);
      }
      select.dataset.previousZone = next;
      const hint = $("[data-calendar-timezone-hint]");
      if (hint) hint.textContent = `Times will be saved in ${calendarZoneOffsetLabel(next)}.`;
    });
    const attendeeInput = $("[data-calendar-attendees]");
    attendeeInput?.addEventListener("input", renderCalendarInviteeSuggestions);
    attendeeInput?.addEventListener("focus", renderCalendarInviteeSuggestions);
    attendeeInput?.addEventListener("keydown", (event) => {
      const list = $("[data-calendar-attendee-suggestions]");
      if (!list || list.hidden) return;
      const options = [...list.querySelectorAll("[data-calendar-attendee-option]")];
      if (event.key === "ArrowDown") { event.preventDefault(); options[0]?.focus(); }
      else if (event.key === "Escape") { list.hidden = true; attendeeInput.setAttribute("aria-expanded", "false"); }
    });
    $("[data-calendar-attendee-suggestions]")?.addEventListener("click", (event) => {
      const option = event.target.closest("[data-calendar-attendee-option]");
      if (option) chooseCalendarInvitee(option.dataset.calendarAttendeeOption);
    });
    $("[data-calendar-content]")?.addEventListener("click", (event) => {
      if (event.target.closest("[data-calendar-agenda-more]")) {
        calendarAgendaLimit += CALENDAR_AGENDA_PAGE;
        const content = $("[data-calendar-content]");
        if (content) renderCalendarAgenda(content, visibleCalendarEvents());
        return;
      }
      const item = event.target.closest("[data-calendar-event]");
      if (item) return openCalendarDialog(calendarEventById(item.dataset.calendarEvent));
      const day = event.target.closest("[data-calendar-open-day]");
      if (day) {
        state.calendarView.anchor = new Date(`${day.dataset.calendarOpenDay}T12:00:00`);
        state.calendarView.mode = "day";
        renderCalendar();
        void loadCalendarRepositoryEvents();
      }
    });
    $("[data-calendar-form]")?.addEventListener("submit", (event) => { event.preventDefault(); void saveCalendarEvent(); });
    for (const selector of ["[data-calendar-close]", "[data-calendar-cancel]"]) $(selector)?.addEventListener("click", closeCalendarDialog);
    $("[data-calendar-delete]")?.addEventListener("click", () => void deleteCalendarEvent());
    $("[data-calendar-rsvp]")?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-calendar-rsvp-value]");
      if (button) void calendarRsvp(button.dataset.calendarRsvpValue);
    });
    document.addEventListener("keydown", (event) => { if (event.key === "Escape" && !$("[data-calendar-modal]")?.hidden) closeCalendarDialog(); });
    renderCalendarList();
    const privateCalendarReady = loadCalendarEvents();
    void Promise.allSettled([
      privateCalendarReady,
      repositoriesReady || Promise.resolve(),
    ]).then(() => loadCalendarRepositoryEvents());
  }

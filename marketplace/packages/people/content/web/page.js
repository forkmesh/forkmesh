(() => {
  const RINGS = Object.freeze([
    { id: "blazing", title: "Blazing", blurb: "Active today and deep in the work", color: "#ff6b2b", icon: "flame" },
    { id: "glowing", title: "Glowing", blurb: "Active in the last five days", color: "#ffa43a", icon: "sun" },
    { id: "kindling", title: "Kindling", blurb: "Joined in the last two weeks", color: "#f4cf6b", icon: "sparkles" },
    { id: "warm", title: "Warm", blurb: "Active this month, or has contributed", color: "#5fc28e", icon: "heart" },
    { id: "embers", title: "Embers", blurb: "Quiet for a while", color: "#7ea6c4", icon: "moon" },
  ]);
  const NEW_WINDOW_MS = 14 * 24 * 60 * 60 * 1000;
  const BLAZING_SCORE = 70;
  const HOUR = 60 * 60 * 1000;
  const RECENCY = { hour: 48, "5h": 46, "24h": 42, "3d": 36, "5d": 32, "10d": 25, "30d": 18, "60d": 11, "120d": 5 };
  const LAST_ACTIVE = {
    hour: "Active in the last hour", "5h": "Active in the last few hours", "24h": "Active today",
    "3d": "Active in the last 3 days", "5d": "Active in the last 5 days", "10d": "Active in the last 10 days",
    "30d": "Active this month", "60d": "Active in the last 2 months", "120d": "Active in the last 4 months",
  };

  const count = (value) => Math.max(0, Math.trunc(Number(value) || 0));
  const tier = (value, tiers) => { for (const [minimum, points] of tiers) if (value >= minimum) return points; return 0; };

  function avatarData(value) {
    const text = String(value || "");
    const comma = text.indexOf(",");
    return text.startsWith("data:image/") && comma > 0 && text.slice(0, comma).endsWith(";base64") ? text : "";
  }

  function parseMember(raw) {
    const name = String(raw?.name || "").trim();
    if (!name || String(raw?.kind || "user") !== "user") return null;
    const status = raw?.userStatus && typeof raw.userStatus === "object" ? raw.userStatus : {};
    return {
      name,
      id: name.toLowerCase(),
      avatar: avatarData(raw?.avatarThumb),
      avatarUrl: String(raw?.avatarUrl || "").startsWith("/api/accounts/") ? String(raw.avatarUrl) : "",
      createdAt: count(raw?.createdAt),
      activityBucket: String(raw?.activityBucket || ""),
      totalActiveMs: count(raw?.totalActiveMs),
      totalSteps: count(raw?.totalSteps),
      officeVisits: count(raw?.officeVisits),
      contributions: ["issues", "pulls", "commits", "discussions"].reduce((sum, key) => sum + count(raw?.[key]), 0),
      nodeCount: Array.isArray(raw?.nodes) ? raw.nodes.length : 0,
      emailVerified: raw?.emailVerified === true,
      hasPayoutAddress: String(raw?.solana || "").trim().length > 0,
      statusText: String(status.text || "").slice(0, 80),
      statusEmoji: String(status.emoji || "").slice(0, 8),
    };
  }

  function score(member) {
    const depth = tier(member.totalActiveMs, [[40 * HOUR, 22], [12 * HOUR, 17], [4 * HOUR, 12], [HOUR, 7], [10 * 60 * 1000, 3]])
      + tier(member.totalSteps, [[100000, 13], [25000, 10], [5000, 7], [500, 4], [1, 1]])
      + tier(member.officeVisits, [[20, 6], [5, 4], [1, 2]])
      + Math.min(14, Math.floor(Math.log2(member.contributions + 1) * 3.5));
    const readiness = (member.emailVerified ? 4 : 0) + (member.hasPayoutAddress ? 4 : 0) + Math.min(7, member.nodeCount * 3);
    return Math.min(100, Math.max(0, (RECENCY[member.activityBucket] || 0) + depth + readiness));
  }

  function ring(member, now) {
    const bucket = member.activityBucket;
    const today = ["hour", "5h", "24h"].includes(bucket);
    if (today && score(member) >= BLAZING_SCORE) return "blazing";
    if (today || ["3d", "5d"].includes(bucket)) return "glowing";
    if (member.createdAt > 0 && now - member.createdAt < NEW_WINDOW_MS) return "kindling";
    if (["10d", "30d"].includes(bucket) || member.contributions > 0 || member.totalSteps > 0 || member.officeVisits > 0) return "warm";
    return "embers";
  }

  function sections(members, now) {
    const ranked = members.map((member) => ({ member, ring: ring(member, now), score: score(member) }));
    return RINGS.map((spec) => ({
      ring: spec.id,
      people: ranked.filter((entry) => entry.ring === spec.id).sort((a, b) =>
        b.score - a.score || b.member.totalActiveMs - a.member.totalActiveMs || (a.member.id < b.member.id ? -1 : a.member.id > b.member.id ? 1 : 0)),
    }));
  }

  function radius(index, size) {
    const half = size / 2;
    const inner = half * 0.26;
    return inner + ((half * 0.9 - inner) / (RINGS.length - 1)) * index;
  }

  function labelGap(index, size, seat, label) {
    return Math.asin(Math.min(1, (label / 2 + seat / 2 + 2) / radius(index, size)));
  }

  function capacity(index, size, seat, label) {
    const arc = 2 * Math.PI - 2 * labelGap(index, size, seat, label);
    return Math.max(2, Math.floor((arc * radius(index, size)) / (seat * 1.15)));
  }

  function seats(list, size, seat, label) {
    const out = [];
    list.forEach((section) => {
      if (!section.people.length) return;
      const index = RINGS.findIndex((spec) => spec.id === section.ring);
      const cap = capacity(index, size, seat, label);
      const overflow = section.people.length > cap ? section.people.length - (cap - 1) : 0;
      const shown = overflow ? section.people.slice(0, cap - 1) : section.people;
      const total = shown.length + (overflow ? 1 : 0);
      const r = radius(index, size);
      const arc = 2 * Math.PI - 2 * labelGap(index, size, seat, label);
      const spread = Math.min(arc, (total * seat * 1.15) / r);
      const start = Math.PI / 2 - spread / 2;
      for (let slot = 0; slot < total; slot += 1) {
        const angle = start + (spread * (slot + 0.5)) / total;
        const person = slot < shown.length ? shown[slot] : null;
        out.push({ ring: section.ring, x: size / 2 + r * Math.cos(angle), y: size / 2 + r * Math.sin(angle), person, overflow: person ? 0 : overflow });
      }
    });
    return out;
  }

  window.ForkMeshPeopleRings = Object.freeze({ RINGS, parseMember, score, ring, sections, radius, labelGap, capacity, seats });

  if (!window.ForkMeshDashboard?.registerPage) return;

  window.ForkMeshDashboard.registerPage("people", ({ select: $, session, escapeHtml }) => {
    const SIZE = 560;
    const SEAT = 36;
    const LABEL = 104;
    const state = { members: [], focus: "", query: "", picked: null, profile: null, busy: false, actionError: "" };
    const spec = (id) => RINGS.find((entry) => entry.id === id) || RINGS[RINGS.length - 1];
    const me = () => String(session()?.nodeName || "").toLowerCase();
    const token = () => { const value = String(session()?.sessionToken || ""); return value && value !== "cookie" ? value : ""; };
    const hue = (name) => { let h = 0; for (const ch of name) h = (h * 31 + ch.codePointAt(0)) % 360; return h; };
    const initial = (name) => escapeHtml((Array.from(name)[0] || "?").toUpperCase());
    const icons = () => window.lucide?.createIcons?.();

    async function request(path, method = "GET") {
      const headers = { accept: "application/json" };
      const bearer = token();
      if (bearer) headers.authorization = `Bearer ${bearer}`;
      let body;
      if (method !== "GET") { headers["content-type"] = "application/json"; body = JSON.stringify(bearer ? { sessionToken: bearer } : {}); }
      const response = await fetch(path, { method, headers, body, credentials: "same-origin", cache: method === "GET" ? "default" : "no-store" });
      const payload = await response.json().catch(() => ({}));
      if (!response.ok || payload.ok === false) {
        const error = new Error(String(payload.error || `HTTP ${response.status}`));
        error.status = response.status; error.code = String(payload.error || ""); throw error;
      }
      return payload;
    }

    function avatarHtml(member, color, size = "") {
      const src = member.avatar || member.avatarUrl;
      const style = `--pp-color:${color};background:hsl(${hue(member.id)} 55% 42%);${size}`;
      return `<span class="pp-avatar" style="${style}" aria-hidden="true">${src ? `<img src="${escapeHtml(src)}" alt="" loading="lazy" decoding="async">` : initial(member.name)}</span>`;
    }

    function subtitle(member) {
      const status = [member.statusEmoji, member.statusText].filter(Boolean).join(" ");
      return status || LAST_ACTIVE[member.activityBucket] || "Not seen recently";
    }

    function currentSections() {
      return sections(state.members, Date.now());
    }

    function visibleSections(all) {
      const needle = state.query.trim().toLowerCase().replace(/^@/, "");
      return all
        .filter((section) => !state.focus || section.ring === state.focus)
        .map((section) => ({ ...section, people: needle ? section.people.filter((entry) => entry.member.id.includes(needle)) : section.people }));
    }

    function flameSvg(cx, cy) {
      return `<g aria-hidden="true">
        <circle class="pp-glow" cx="${cx}" cy="${cy}" r="70" fill="url(#pp-glow)"/>
        <g transform="translate(${cx - 30} ${cy - 44})">
          <path d="M10 70 L50 70 L56 76 L4 76 Z" fill="#5a2c12"/>
          <path d="M2 74 L58 66 L60 72 L4 80 Z" fill="#6e3716"/>
          <path class="pp-flame" d="M30 0 C40 18 56 26 52 48 C50 62 40 70 30 70 C18 70 8 62 8 48 C8 36 16 30 20 20 C22 30 26 32 30 34 C30 22 26 12 30 0 Z" fill="url(#pp-flame)"/>
          <path class="pp-flame pp-flame-core" d="M30 30 C36 40 44 46 42 56 C41 64 36 68 30 68 C23 68 18 63 18 56 C18 48 24 44 30 30 Z" fill="#fff3b0"/>
        </g>
      </g>`;
    }

    function renderFire(all) {
      const box = $("[data-people-fire]");
      if (!box) return;
      const c = SIZE / 2;
      const bands = RINGS.map((entry, index) => {
        const r = radius(index, SIZE);
        const dim = state.focus && state.focus !== entry.id ? 1 : 0;
        const active = state.focus === entry.id;
        return `<g data-dim="${dim}">
          <circle class="pp-band" cx="${c}" cy="${c}" r="${r}" stroke="${entry.color}" stroke-opacity=".09" stroke-width="${SIZE * 0.06}"/>
          <circle class="pp-band" cx="${c}" cy="${c}" r="${r}" stroke="${entry.color}" stroke-opacity=".6" stroke-width="${active ? 3 : 1.5}" ${entry.id === "embers" ? 'stroke-dasharray="5 6"' : ""} ${index < 2 ? 'filter="url(#pp-soft)"' : ""}/>
        </g>`;
      }).join("");
      const seatHtml = seats(all, SIZE, SEAT, LABEL).map((seat) => {
        const color = spec(seat.ring).color;
        const dim = state.focus && state.focus !== seat.ring ? 1 : 0;
        const r = SEAT / 2;
        if (!seat.person) {
          return `<g class="pp-seat" data-dim="${dim}" data-people-ring="${seat.ring}" role="button" tabindex="0" aria-label="${seat.overflow} more in ${spec(seat.ring).title}">
            <circle cx="${seat.x}" cy="${seat.y}" r="${r}" fill="${color}"/>
            <text x="${seat.x}" y="${seat.y}" text-anchor="middle" dominant-baseline="central" font-size="13" font-weight="700" fill="#120d14">+${seat.overflow}</text>
          </g>`;
        }
        const member = seat.person.member;
        const src = member.avatar || member.avatarUrl;
        const clip = `pp-clip-${member.id.replace(/[^a-z0-9_-]/g, "")}`;
        const mine = member.id === me();
        return `<g class="pp-seat" data-dim="${dim}" data-people-person="${escapeHtml(member.id)}" role="button" tabindex="0" aria-label="@${escapeHtml(member.name)}, ${spec(seat.ring).title}">
          <title>@${escapeHtml(member.name)} · ${spec(seat.ring).title}</title>
          <clipPath id="${clip}"><circle cx="${seat.x}" cy="${seat.y}" r="${r}"/></clipPath>
          <circle cx="${seat.x}" cy="${seat.y}" r="${r}" fill="hsl(${hue(member.id)} 55% 42%)"/>
          <text x="${seat.x}" y="${seat.y}" text-anchor="middle" dominant-baseline="central" font-size="17" font-weight="700" fill="#fff">${initial(member.name)}</text>
          ${src ? `<image href="${escapeHtml(src)}" x="${seat.x - r}" y="${seat.y - r}" width="${SEAT}" height="${SEAT}" clip-path="url(#${clip})" preserveAspectRatio="xMidYMid slice"/>` : ""}
          <circle class="pp-ring" cx="${seat.x}" cy="${seat.y}" r="${r}" fill="none" stroke="${mine ? "#fff" : color}" stroke-width="2.5" ${seat.ring === "blazing" ? 'filter="url(#pp-soft)"' : ""}/>
        </g>`;
      }).join("");
      const labels = RINGS.map((entry, index) => {
        const r = radius(index, SIZE);
        const total = all.find((section) => section.ring === entry.id)?.people.length || 0;
        const active = state.focus === entry.id;
        const text = `${entry.title} ${total}`;
        const width = Math.min(LABEL, 22 + text.length * 7);
        return `<g class="pp-label" data-people-ring="${entry.id}" role="button" tabindex="0" aria-pressed="${active}" aria-label="${entry.title} ring, ${total} ${total === 1 ? "person" : "people"}">
          <rect x="${c - width / 2}" y="${c - r - 11}" width="${width}" height="22" rx="11" fill="${active ? entry.color : "#120d14"}" fill-opacity="${active ? 1 : 0.88}" stroke="${entry.color}" stroke-opacity=".75"/>
          <text x="${c}" y="${c - r}" text-anchor="middle" dominant-baseline="central" font-size="12" font-weight="600" fill="${active ? "#120d14" : entry.color}">${entry.title} <tspan font-weight="400" opacity=".8">${total}</tspan></text>
        </g>`;
      }).join("");
      box.innerHTML = `<svg viewBox="0 0 ${SIZE} ${SIZE}" role="group" aria-label="Campfire of people, from the most active at the centre to the quietest at the edge">
        <defs>
          <radialGradient id="pp-glow"><stop offset="0" stop-color="#ff8a2b" stop-opacity=".55"/><stop offset="1" stop-color="#ff8a2b" stop-opacity="0"/></radialGradient>
          <linearGradient id="pp-flame" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffd85a"/><stop offset=".6" stop-color="#ff7a1f"/><stop offset="1" stop-color="#e2430f"/></linearGradient>
          <filter id="pp-soft" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="2.5" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>
        </defs>
        ${bands}${flameSvg(c, c)}${seatHtml}${labels}
      </svg>`;
    }

    function renderLegend(all) {
      const box = $("[data-people-legend]");
      if (!box) return;
      box.innerHTML = all.map((section) => {
        const entry = spec(section.ring);
        return `<button type="button" class="pp-chip" style="--pp-color:${entry.color}" data-people-ring="${entry.id}" aria-pressed="${state.focus === entry.id}">
          <span class="pp-dot"></span>${entry.title}<span class="pp-count">${section.people.length}</span></button>`;
      }).join("");
    }

    function renderSections(all) {
      const box = $("[data-people-sections]");
      if (!box) return;
      const visible = visibleSections(all);
      const mine = me();
      const html = visible.map((section) => {
        const entry = spec(section.ring);
        const people = section.people.map(({ member }) => `<button type="button" class="pp-person" style="--pp-color:${entry.color}" data-people-person="${escapeHtml(member.id)}">
            ${avatarHtml(member, entry.color)}
            <span class="min-w-0"><span class="pp-name">@${escapeHtml(member.name)}${member.id === mine ? '<span class="pp-you">You</span>' : ""}</span><span class="pp-sub">${escapeHtml(subtitle(member))}</span></span>
          </button>`).join("");
        return `<section class="pp-section" style="--pp-color:${entry.color}" aria-labelledby="pp-head-${entry.id}">
          <header class="pp-section-head">
            <span class="pp-badge"><i data-lucide="${entry.icon}" class="h-4 w-4"></i></span>
            <span class="min-w-0"><h2 id="pp-head-${entry.id}" class="text-sm font-semibold text-foreground">${entry.title}</h2><p class="text-xs text-muted-foreground">${entry.blurb}</p></span>
            <span class="pp-section-count">${section.people.length}</span>
          </header>
          ${people ? `<div class="pp-grid">${people}</div>` : `<p class="px-5 py-3 text-xs text-muted-foreground">${state.query ? "No one in this ring matches." : "No one in this ring right now."}</p>`}
        </section>`;
      }).join("");
      box.innerHTML = html;
      icons();
    }

    function render() {
      const all = currentSections();
      renderFire(all);
      renderLegend(all);
      renderSections(all);
    }

    function find(id) {
      for (const section of currentSections()) {
        const hit = section.people.find((entry) => entry.member.id === id);
        if (hit) return hit;
      }
      return null;
    }

    function messageHref(name) {
      const official = new Set(["app.forkmesh.com", "forkmesh.com", "world.forkmesh.com"]).has(location.hostname.toLowerCase());
      return `${official ? "https://app.forkmesh.com" : ""}/chat?dm=${encodeURIComponent(name)}`;
    }

    function renderDialog() {
      const body = $("[data-people-dialog-body]");
      const picked = state.picked;
      if (!body || !picked) return;
      const entry = spec(picked.ring);
      const member = picked.member;
      const profile = state.profile;
      const mine = me();
      const self = mine && mine === member.id;
      const avatar = profile?.avatarPng ? { ...member, avatar: `data:image/png;base64,${profile.avatarPng}` } : member;
      const social = profile?.social && typeof profile.social === "object" ? profile.social : profile || {};
      const following = social.isFollowing === true;
      const joined = count(profile?.createdAt || member.createdAt);
      const status = [profile?.userStatus?.emoji, profile?.userStatus?.text].filter(Boolean).join(" ");
      let actions = "";
      if (self) actions = `<p class="text-sm text-muted-foreground">This is you.</p>`;
      else if (!mine) actions = `<a class="pp-btn pp-btn-primary" href="/login">Sign in to follow or message</a>`;
      else actions = `<button type="button" class="pp-btn ${following ? "pp-btn-outline" : "pp-btn-primary"}" data-people-follow ${state.busy || !profile ? "disabled" : ""} aria-label="${following ? "Unfollow" : "Follow"} @${escapeHtml(member.name)}">
          <i data-lucide="${following ? "user-check" : "user-plus"}" class="h-4 w-4"></i>${state.busy ? "…" : following ? "Following" : "Follow"}</button>
        <a class="pp-btn pp-btn-outline" data-people-message href="${escapeHtml(messageHref(member.name))}" aria-label="Message @${escapeHtml(member.name)}"><i data-lucide="message-circle" class="h-4 w-4"></i>Message</a>`;
      body.innerHTML = `<div style="--pp-color:${entry.color}">
        <div class="pp-hero">
          <button type="button" class="pp-close" data-people-close aria-label="Close"><i data-lucide="x" class="h-4 w-4"></i></button>
          ${avatarHtml(avatar, entry.color)}
          <h2 id="pp-dialog-name" class="text-xl font-semibold">@${escapeHtml(member.name)}</h2>
          <span class="pp-ring-badge"><i data-lucide="${entry.icon}" class="h-3 w-3"></i>${entry.title} ring</span>
          <span class="text-xs" style="opacity:.75">${escapeHtml(LAST_ACTIVE[member.activityBucket] || "Not seen recently")}</span>
        </div>
        <div class="flex flex-col gap-4 p-4">
          <div class="flex gap-3">${actions}</div>
          ${state.actionError ? `<p class="text-sm text-destructive">${escapeHtml(state.actionError)}</p>` : ""}
          ${profile?.profilePrivate ? `<p class="text-sm text-muted-foreground">This profile is private.</p>` : `
          <div class="pp-stats">
            <div><strong>${profile ? count(social.followers) : "–"}</strong><span>Followers</span></div>
            <div><strong>${profile ? count(social.following) : "–"}</strong><span>Following</span></div>
            <div><strong>${joined ? escapeHtml(new Date(joined).toLocaleDateString(undefined, { month: "short", year: "numeric" })) : "–"}</strong><span>Joined</span></div>
          </div>
          ${status ? `<p class="text-sm">${escapeHtml(status)}</p>` : ""}
          ${profile?.profileBio ? `<p class="text-sm text-foreground">${escapeHtml(String(profile.profileBio).slice(0, 500))}</p>` : ""}
          ${profile?.profileLocation ? `<p class="flex items-center gap-1 text-xs text-muted-foreground"><i data-lucide="map-pin" class="h-3 w-3"></i>${escapeHtml(String(profile.profileLocation).slice(0, 120))}</p>` : ""}
          ${!profile && !state.actionError ? `<p class="text-sm text-muted-foreground">Loading profile…</p>` : ""}`}
          <a class="flex min-h-[2.75rem] items-center justify-between text-sm font-semibold text-primary" href="/@${encodeURIComponent(member.name)}">View full profile<i data-lucide="chevron-right" class="h-4 w-4"></i></a>
        </div>
      </div>`;
      icons();
    }

    async function open(id) {
      const picked = find(id);
      const dialog = $("[data-people-dialog]");
      if (!picked || !dialog) return;
      state.picked = picked; state.profile = null; state.actionError = ""; state.busy = false;
      renderDialog();
      if (!dialog.open) dialog.showModal();
      const viewer = me();
      try {
        const profile = await request(`/api/accounts/${encodeURIComponent(picked.member.id)}${viewer ? `?viewer=${encodeURIComponent(viewer)}` : ""}`);
        if (state.picked?.member.id === id) { state.profile = profile; renderDialog(); }
      } catch (error) {
        if (state.picked?.member.id === id) { state.actionError = "Could not load this profile."; renderDialog(); }
      }
    }

    async function toggleFollow() {
      const picked = state.picked;
      if (!picked || !state.profile || state.busy) return;
      const social = state.profile.social && typeof state.profile.social === "object" ? state.profile.social : state.profile;
      const follow = social.isFollowing !== true;
      state.busy = true; state.actionError = ""; renderDialog();
      try {
        const result = await request(`/api/accounts/${encodeURIComponent(picked.member.id)}/follow`, follow ? "POST" : "DELETE");
        const followers = result.followers ?? Math.max(0, count(social.followers) + (follow ? 1 : -1));
        const isFollowing = result.isFollowing ?? follow;
        state.profile = { ...state.profile, followers, isFollowing, social: { ...social, followers, isFollowing } };
      } catch (error) {
        state.actionError = error.code === "cannot_follow_self" ? "You cannot follow yourself."
          : error.status === 401 ? "Sign in again to follow people."
          : error.status === 403 || error.code === "email_not_verified" ? "Verify your email to follow people."
          : follow ? "Could not follow." : "Could not unfollow.";
      }
      state.busy = false;
      if (state.picked === picked) renderDialog();
    }

    async function load() {
      const error = $("[data-people-error]");
      try {
        const payload = await request("/api/accounts/users?view=world");
        const seen = new Set();
        state.members = (Array.isArray(payload.users) ? payload.users : []).map(parseMember).filter((member) => member && !seen.has(member.id) && seen.add(member.id));
        error?.classList.add("hidden");
        $("[data-people-loading]")?.classList.add("hidden");
        $("[data-people-body]")?.classList.remove("hidden");
        render();
      } catch (failure) {
        if (error) { error.textContent = "Could not load people. Check your connection and try again."; error.classList.remove("hidden"); }
        $("[data-people-loading]")?.classList.add("hidden");
      }
    }

    function focusRing(id) {
      state.focus = state.focus === id ? "" : id;
      render();
    }

    function activate(target) {
      const person = target.closest("[data-people-person]");
      if (person) { void open(person.dataset.peoplePerson); return true; }
      const ringTarget = target.closest("[data-people-ring]");
      if (ringTarget) { focusRing(ringTarget.dataset.peopleRing); return true; }
      return false;
    }

    function init() {
      const app = $("[data-people-app]");
      app?.addEventListener("click", (event) => { activate(event.target); });
      app?.addEventListener("keydown", (event) => {
        if ((event.key === "Enter" || event.key === " ") && event.target.closest?.("svg") && activate(event.target)) event.preventDefault();
      });
      $("[data-people-query]")?.addEventListener("input", (event) => { state.query = event.target.value || ""; renderSections(currentSections()); });
      const dialog = $("[data-people-dialog]");
      dialog?.addEventListener("click", (event) => {
        if (event.target === dialog || event.target.closest("[data-people-close]")) { dialog.close(); return; }
        if (event.target.closest("[data-people-follow]")) void toggleFollow();
      });
      dialog?.addEventListener("close", () => { state.picked = null; state.profile = null; });
      void load();
    }

    return init();
  });
})();

window.ForkMeshDashboard.registerPage("stretch", ({ select: $, selectAll: $$, session, escapeHtml }) => {
  const SVG = "http://www.w3.org/2000/svg";
  const state = { installed: false, overview: null, library: null, routine: "desk", voice: true, run: null, preview: null, timer: 0, frame: 0 };
  const currentSession = () => session();
  const speech = window.speechSynthesis || null;

  function localDay(date) {
    const d = new Date(date.getTime() - date.getTimezoneOffset() * 60000);
    return d.toISOString().slice(0, 10);
  }
  function shortDay(day) { return new Date(day + "T00:00:00").toLocaleDateString(undefined, { weekday: "short", month: "short", day: "numeric" }); }
  function plural(count, one, many) { return `${count} ${count === 1 ? one : many}`; }
  function minutes(seconds) {
    const value = Number(seconds || 0);
    if (value < 60) return plural(Math.round(value), "second", "seconds");
    const whole = Math.floor(value / 60), rest = Math.round(value % 60);
    return rest ? `${whole} min ${rest} s` : `${whole} min`;
  }
  function message(kind, value = "") {
    const node = $(`[data-stretch-${kind}]`); if (!node) return;
    node.textContent = value; node.classList.toggle("hidden", !value);
  }
  function apiPath(suffix = "") {
    return `/api/stretch${suffix}${suffix.includes("?") ? "&" : "?"}day=${localDay(new Date())}`;
  }
  function downloadJson(payload, filename) {
    const url = URL.createObjectURL(new Blob([JSON.stringify(payload, null, 2)], { type: "application/json" }));
    const link = document.createElement("a"); link.href = url; link.download = filename; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  async function request(path, method = "GET", body = null) {
    const headers = { accept: "application/json" };
    if (currentSession()?.sessionToken) headers.authorization = `Bearer ${currentSession().sessionToken}`;
    if (body !== null) headers["content-type"] = "application/json";
    const response = await fetch(path, { method, headers, cache: "no-store", body: body === null ? undefined : JSON.stringify(body) });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(String(payload.error || `HTTP ${response.status}`).replaceAll("_", " "));
      error.status = response.status; error.payload = payload; throw error;
    }
    return payload;
  }

  function pickVoice() {
    if (!speech) return null;
    const voices = speech.getVoices();
    const lang = (navigator.language || "en").toLowerCase();
    const scored = voices.filter((voice) => voice.lang.toLowerCase().startsWith(lang.slice(0, 2))).map((voice) => {
      let score = 0;
      if (voice.lang.toLowerCase() === lang) score += 4;
      if (/natural|neural|premium|enhanced/i.test(voice.name)) score += 6;
      if (/google|samantha|siri|aria|jenny|daniel/i.test(voice.name)) score += 3;
      if (voice.localService) score += 1;
      return [score, voice];
    }).sort((a, b) => b[0] - a[0]);
    return scored[0]?.[1] || null;
  }
  function say(text) {
    if (!speech || !state.voice || !text) return;
    speech.cancel();
    const utterance = new SpeechSynthesisUtterance(text);
    const voice = pickVoice(); if (voice) utterance.voice = voice;
    utterance.rate = 0.95; utterance.pitch = 1;
    speech.speak(utterance);
  }
  function hush() { speech?.cancel(); }

  function routine(id = state.routine) { return (state.library?.routines || []).find((item) => item.id === id) || state.library?.routines?.[0] || null; }
  function pose(motion, seconds) {
    const frames = motion.frames, fields = state.library.poseFields;
    const phase = (((seconds % motion.period) + motion.period) % motion.period) / motion.period;
    let a = frames[0], b = frames[frames.length - 1];
    for (let index = 0; index < frames.length - 1; index += 1) {
      if (frames[index].t <= phase && phase <= frames[index + 1].t) { a = frames[index]; b = frames[index + 1]; break; }
    }
    let u = (phase - a.t) / Math.max(1e-9, b.t - a.t); u = u * u * (3 - 2 * u);
    const result = {};
    fields.forEach((field) => { result[field] = a[field] + (b[field] - a[field]) * u; });
    return result;
  }
  function rotate(deg, x, y) {
    const r = deg * Math.PI / 180;
    return [x * Math.cos(r) - y * Math.sin(r), x * Math.sin(r) + y * Math.cos(r)];
  }
  function geometry(p, tap, seconds) {
    const F = state.library.figure, rad = Math.PI / 180;
    const hip = [F.hip[0] + p.hipX * F.hipShift, F.hip[1] + p.knees * F.kneeDip];
    const length = F.torso * (1 - 0.5 * p.fold);
    const up = rotate(p.lean, 0, -1);
    const chest = [hip[0] + up[0] * length, hip[1] + up[1] * length];
    const width = F.shoulderHalf * (1 - 0.4 * Math.abs(p.twist));
    const side = rotate(p.lean, 1, 0);
    const lift = -p.shrug * F.shrug;
    const shoulders = { l: [chest[0] - side[0] * width, chest[1] - side[1] * width + lift], r: [chest[0] + side[0] * width, chest[1] + side[1] * width + lift] };
    const tilt = p.lean + p.headTilt;
    const headUp = rotate(tilt, 0, -1);
    const head = [chest[0] + headUp[0] * F.neck + p.twist * 3, chest[1] + headUp[1] * F.neck + p.headNod * 6];
    const eyes = [-5, 5].map((dx) => { const e = rotate(tilt, dx + p.headTurn * 5, -2 + p.headNod * 3); return [head[0] + e[0], head[1] + e[1]]; });
    const arms = {};
    const tapping = [];
    [["l", -1], ["r", 1]].forEach(([key, sign]) => {
      const arm = p[key + "Arm"], elbowAngle = p[key + "Elbow"], shoulder = shoulders[key];
      const v1 = rotate(p.lean, sign * Math.sin(arm * rad), Math.cos(arm * rad));
      const elbow = [shoulder[0] + v1[0] * F.upperArm, shoulder[1] + v1[1] * F.upperArm];
      const v2 = rotate(p.lean, sign * Math.sin((arm + elbowAngle) * rad), Math.cos((arm + elbowAngle) * rad));
      const hand = [elbow[0] + v2[0] * F.forearm, elbow[1] + v2[1] * F.forearm];
      if (tap === "both" || tap === (key === "l" ? "left" : "right")) {
        hand[1] -= F.tapLift * Math.abs(Math.sin(2 * Math.PI * F.tapHz * seconds));
        tapping.push(hand);
      }
      arms[key] = { shoulder, elbow, hand };
    });
    const legs = [[0, -1], [1, 1]].map(([index, sign]) => {
      const top = [hip[0] + sign * F.hipHalf, hip[1]], foot = F.feet[index];
      return { top, knee: [(top[0] + foot[0]) / 2 + sign * p.knees * 10, (top[1] + foot[1]) / 2], foot };
    });
    return { hip, chest, shoulders, head, eyes, arms, legs, tapping, radius: F.head };
  }
  function stage(container) {
    const F = state.library.figure;
    const svg = document.createElementNS(SVG, "svg");
    svg.setAttribute("viewBox", `0 0 ${F.width} ${F.height}`);
    svg.setAttribute("role", "img");
    const make = (tag, attrs) => { const node = document.createElementNS(SVG, tag); Object.entries(attrs).forEach(([k, v]) => node.setAttribute(k, v)); svg.appendChild(node); return node; };
    make("ellipse", { cx: F.width / 2, cy: F.feet[0][1] + 4, rx: 46, ry: 5, fill: "var(--stretch-track)" });
    const line = (width) => make("polyline", { class: "stretch-limb", "stroke-width": width, "stroke-linejoin": "round" });
    const parts = {
      legs: [line(10), line(10)], torso: line(14), shoulders: line(10),
      arms: { l: line(9), r: line(9) },
      head: make("circle", { r: F.head, fill: "var(--stretch-figure)" }),
      eyes: [make("circle", { r: 2, fill: "var(--stretch-bg)" }), make("circle", { r: 2, fill: "var(--stretch-bg)" })],
      ripples: [make("circle", { class: "stretch-ripple" }), make("circle", { class: "stretch-ripple" })],
    };
    container.replaceChildren(svg);
    return { svg, parts };
  }
  function points(list) { return list.map(([x, y]) => `${x.toFixed(1)},${y.toFixed(1)}`).join(" "); }
  function draw(view, motionId, seconds, label) {
    const motion = state.library?.motions?.[motionId]; if (!view || !motion) return;
    const g = geometry(pose(motion, seconds), motion.tap, seconds), parts = view.parts;
    g.legs.forEach((leg, index) => parts.legs[index].setAttribute("points", points([leg.top, leg.knee, leg.foot])));
    parts.torso.setAttribute("points", points([g.hip, g.chest]));
    parts.shoulders.setAttribute("points", points([g.shoulders.l, g.shoulders.r]));
    ["l", "r"].forEach((key) => parts.arms[key].setAttribute("points", points([g.arms[key].shoulder, g.arms[key].elbow, g.arms[key].hand])));
    parts.head.setAttribute("cx", g.head[0].toFixed(1)); parts.head.setAttribute("cy", g.head[1].toFixed(1));
    g.eyes.forEach((eye, index) => { parts.eyes[index].setAttribute("cx", eye[0].toFixed(1)); parts.eyes[index].setAttribute("cy", eye[1].toFixed(1)); });
    const pulse = (seconds * state.library.figure.tapHz) % 1;
    parts.ripples.forEach((ring, index) => {
      const hand = g.tapping[index];
      ring.setAttribute("visibility", hand ? "visible" : "hidden");
      if (!hand) return;
      ring.setAttribute("cx", hand[0].toFixed(1)); ring.setAttribute("cy", hand[1].toFixed(1));
      ring.setAttribute("r", (4 + pulse * 8).toFixed(1)); ring.setAttribute("opacity", (1 - pulse).toFixed(2));
    });
    if (label) view.svg.setAttribute("aria-label", label);
  }

  function renderInstall() {
    $(`[data-stretch-install-panel]`)?.classList.toggle("hidden", state.installed);
    $(`[data-stretch-app]`)?.classList.toggle("hidden", !state.installed);
    $$(`[data-stretch-requires-install]`).forEach((node) => { node.classList.toggle("hidden", !state.installed); if (node.tagName === "LABEL") node.classList.toggle("flex", state.installed); });
  }
  function renderPicker() {
    const routines = state.library?.routines || [];
    $(`[data-stretch-routines]`).innerHTML = routines.map((item) => `<button type="button" class="stretch-routine" data-stretch-routine="${escapeHtml(item.id)}" aria-pressed="${item.id === state.routine}">
        <span class="block text-sm font-semibold text-foreground">${escapeHtml(item.title)}</span>
        <span class="block text-xs text-muted-foreground">${escapeHtml(minutes(item.seconds))} · ${plural(item.steps.length, "step", "steps")}</span>
        <span class="mt-1 block text-xs text-muted-foreground">${escapeHtml(item.summary)}</span>
      </button>`).join("");
    const chosen = routine();
    $(`[data-stretch-step-list]`).innerHTML = (chosen?.steps || []).map((step, index) => `<li>${index + 1}. ${escapeHtml(step.title)} <span class="text-xs">(${step.seconds} s)</span></li>`).join("");
    $(`[data-stretch-start]`).textContent = chosen ? `Start ${chosen.title.toLowerCase()} · ${minutes(chosen.seconds)}` : "Start";
  }
  function renderStats() {
    const overview = state.overview || {}, summary = overview.summary || {}, today = overview.today || {};
    const stats = [
      ["Today", minutes(today.seconds || 0), plural(Number(today.sessions || 0), "session", "sessions")],
      ["Streak", plural(Number(summary.streak || 0), "day", "days"), `best ${plural(Number(summary.bestStreak || 0), "day", "days")}`],
      ["This week", minutes(summary.weekSeconds || 0), plural(Number(summary.weekSessions || 0), "session", "sessions")],
      ["All time", plural(Number(summary.sessions || 0), "session", "sessions"), "on every device"],
    ];
    $(`[data-stretch-stats]`).innerHTML = stats.map(([label, value, note]) => `<div><div class="text-xs text-muted-foreground">${escapeHtml(label)}</div><div class="text-base font-semibold text-foreground">${escapeHtml(value)}</div><div class="text-xs text-muted-foreground">${escapeHtml(note)}</div></div>`).join("");
    const days = overview.days || [];
    const top = Math.max(300, ...days.map((day) => Number(day.seconds || 0)));
    const width = 14 * 22;
    $(`[data-stretch-chart]`).innerHTML = `<svg viewBox="0 0 ${width} 70" style="width:100%;height:auto" role="img" aria-label="Minutes stretched on each of the last 14 days">
      ${days.map((day, index) => {
        const height = Math.max(day.seconds ? 3 : 1, 52 * Number(day.seconds || 0) / top);
        return `<rect x="${index * 22 + 3}" y="${54 - height}" width="16" height="${height}" rx="3" fill="${day.seconds ? "var(--stretch-accent)" : "var(--stretch-track)"}"><title>${escapeHtml(`${shortDay(day.day)}: ${minutes(day.seconds)}`)}</title></rect>
        <text x="${index * 22 + 11}" y="68" text-anchor="middle" font-size="9" fill="var(--stretch-ink-muted)">${Number(day.day.slice(8))}</text>`;
      }).join("")}
    </svg>`;
    const recent = overview.recent || [];
    $(`[data-stretch-recent-panel]`).classList.toggle("hidden", !recent.length);
    $(`[data-stretch-recent]`).innerHTML = recent.map((item) => {
      const title = routine(item.routine)?.title || item.routine;
      const time = new Date(Number(item.at || item.loggedAt || 0)).toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" });
      const detail = `${minutes(item.seconds)} · ${item.completed ? "finished" : `${item.steps} of ${item.stepsTotal} steps`} · ${item.source}${item.health ? " · ♥ Apple Health" : ""}`;
      return `<div class="flex flex-wrap items-center justify-between gap-2 rounded-md px-2 py-1.5 hover:bg-secondary">
        <span class="text-sm text-foreground">${escapeHtml(`${shortDay(item.day)} ${time} · ${title}`)}</span>
        <span class="flex items-center gap-3 text-xs text-muted-foreground">${escapeHtml(detail)}<button type="button" data-stretch-delete="${escapeHtml(item.id)}" class="hover:text-destructive">Delete</button></span>
      </div>`;
    }).join("");
  }
  function render() {
    const voice = $(`[data-stretch-voice]`); if (voice) voice.checked = state.voice;
    renderPicker(); renderStats();
    if (!state.preview) state.preview = stage($(`[data-stretch-preview]`));
  }

  function previewMotion() {
    const steps = routine()?.steps || [];
    if (!steps.length) return null;
    const index = Math.floor(performance.now() / 4000) % steps.length;
    return steps[index];
  }
  function animate() {
    state.frame = requestAnimationFrame(animate);
    const seconds = performance.now() / 1000;
    if (state.run) {
      tick();
      const run = state.run; if (!run) return;
      const step = run.routine.steps[run.index];
      draw(run.view, step.motion, run.stepElapsed, `Example person: ${step.title}`);
    } else if (state.preview && state.installed) {
      const step = previewMotion();
      if (step) draw(state.preview, step.motion, seconds, `Example person: ${step.title}`);
    }
  }
  function stopAnimation() { cancelAnimationFrame(state.frame); state.frame = 0; }
  function startAnimation() { if (!state.frame && !document.hidden) state.frame = requestAnimationFrame(animate); }

  function startStep(index) {
    const run = state.run; if (!run) return;
    run.index = index; run.stepElapsed = 0; run.halfSaid = false; run.nextSaid = false;
    const step = run.routine.steps[index];
    say(`${step.title}. ${step.say}`);
    renderRun();
  }
  function tick() {
    const run = state.run; if (!run) return;
    const now = performance.now();
    const delta = Math.min(5, Math.max(0, (now - run.last) / 1000)); run.last = now;
    if (run.paused) return;
    run.stepElapsed += delta; run.active += delta;
    const steps = run.routine.steps;
    const step = steps[run.index];
    const next = steps[run.index + 1];
    if (!run.halfSaid && step.half && step.seconds >= 20 && run.stepElapsed >= step.seconds / 2) { run.halfSaid = true; say(step.half); }
    if (!run.nextSaid && next && step.seconds >= 15 && run.stepElapsed >= step.seconds - 3) { run.nextSaid = true; say(`Next: ${next.title}`); }
    if (run.stepElapsed >= step.seconds) {
      run.done = Math.max(run.done, run.index + 1);
      if (next) startStep(run.index + 1); else void finish(true);
      return;
    }
    renderCountdown();
  }
  function renderCountdown() {
    const run = state.run; if (!run) return;
    const step = run.routine.steps[run.index];
    $(`[data-stretch-count]`).textContent = String(Math.max(0, Math.ceil(step.seconds - run.stepElapsed)));
    $(`[data-stretch-step-bar]`).style.width = `${Math.min(100, 100 * run.stepElapsed / step.seconds).toFixed(1)}%`;
    const before = run.routine.steps.slice(0, run.index).reduce((sum, item) => sum + item.seconds, 0);
    const total = run.routine.seconds;
    $(`[data-stretch-total-bar]`).style.width = `${Math.min(100, 100 * (before + run.stepElapsed) / total).toFixed(1)}%`;
    $(`[data-stretch-total]`).textContent = `${minutes(Math.max(0, total - before - run.stepElapsed))} left of ${run.routine.title.toLowerCase()}`;
  }
  function renderRun() {
    const run = state.run; if (!run) return;
    const steps = run.routine.steps, step = steps[run.index], next = steps[run.index + 1];
    $(`[data-stretch-position]`).textContent = `Step ${run.index + 1} of ${steps.length}`;
    $(`[data-stretch-title]`).textContent = step.title;
    $(`[data-stretch-say]`).textContent = step.say;
    $(`[data-stretch-next]`).textContent = next ? `Next: ${next.title}` : "Last step";
    $(`[data-stretch-pause]`).textContent = run.paused ? "Resume" : "Pause";
    $(`[data-stretch-back]`).disabled = run.index === 0 && run.stepElapsed < 1;
    renderCountdown();
  }
  function startSession() {
    const chosen = routine(); if (!chosen?.steps?.length) return;
    message("notice", "");
    $(`[data-stretch-picker]`).classList.add("hidden");
    $(`[data-stretch-session]`).classList.remove("hidden");
    state.run = { routine: chosen, index: 0, stepElapsed: 0, active: 0, done: 0, paused: false, at: Date.now(), last: performance.now(), view: stage($(`[data-stretch-stage]`)) };
    clearInterval(state.timer); state.timer = setInterval(tick, 250);
    startStep(0);
    startAnimation();
    $(`[data-stretch-session]`).scrollIntoView({ behavior: "smooth", block: "start" });
  }
  function togglePause() {
    const run = state.run; if (!run) return;
    run.paused = !run.paused; run.last = performance.now();
    if (run.paused) hush(); else say(run.routine.steps[run.index].title);
    renderRun();
  }
  function back() {
    const run = state.run; if (!run) return;
    startStep(run.stepElapsed > 3 || run.index === 0 ? run.index : run.index - 1);
  }
  function skip() {
    const run = state.run; if (!run) return;
    if (run.index + 1 < run.routine.steps.length) startStep(run.index + 1); else void finish(false);
  }
  async function finish(completed) {
    const run = state.run; if (!run) return;
    state.run = null; clearInterval(state.timer); state.timer = 0;
    $(`[data-stretch-session]`).classList.add("hidden");
    $(`[data-stretch-picker]`).classList.remove("hidden");
    const seconds = Math.round(run.active);
    if (completed) say(`Well done. That is your ${run.routine.title.toLowerCase()} finished.`); else hush();
    if (!completed && seconds < 30) { message("notice", "Stopped. Sessions shorter than 30 seconds are not logged."); return; }
    try {
      await request(apiPath("/sessions"), "POST", {
        routine: run.routine.id, seconds: Math.max(1, seconds), steps: completed ? run.routine.steps.length : run.done,
        stepsTotal: run.routine.steps.length, completed, at: run.at, day: localDay(new Date(run.at)), source: "web",
      });
      message("notice", completed ? `Well done: ${minutes(seconds)} of ${run.routine.title.toLowerCase()} logged.` : `Stopped and logged ${minutes(seconds)}.`);
      await load();
    } catch (error) { message("error", `Could not log the session: ${error.message}`); }
  }

  async function load() {
    try {
      const payload = await request(apiPath()); state.installed = payload.installed !== false;
      state.library = payload.library || state.library;
      state.overview = state.installed ? payload : null; message("error", "");
      if (state.overview) { state.voice = payload.profile?.voice !== false; if (!state.run) state.routine = payload.profile?.routine || state.routine; }
      renderInstall(); if (state.installed && state.library) render();
    } catch (error) {
      if (error.status === 409) { state.installed = false; state.overview = null; renderInstall(); }
      else message("error", `Stretch Break is unavailable: ${error.message}`);
    }
  }
  async function saveProfile(change) {
    try { const payload = await request(apiPath("/profile"), "PUT", change); state.voice = payload.profile?.voice !== false; }
    catch (error) { message("error", `Could not save: ${error.message}`); }
  }
  async function install() {
    const button = $(`[data-stretch-install]`); if (button) button.disabled = true;
    try { await request(apiPath("/install"), "POST", { tzOffsetMinutes: new Date().getTimezoneOffset() }); message("notice", "Stretch Break installed. Pick a routine and press Start; it follows your account to the desktop and iPhone apps."); await load(); }
    catch (error) { message("error", `Could not install: ${error.message}`); }
    finally { if (button) button.disabled = false; }
  }
  async function deleteSession(id) {
    if (!confirm("Delete this session from your log? Apple Health keeps its own copy.")) return;
    try { await request(apiPath(`/sessions/${encodeURIComponent(id)}`), "DELETE"); message("notice", "Session deleted."); await load(); }
    catch (error) { message("error", `Could not delete: ${error.message}`); }
  }
  async function exportMine() {
    try { const snapshot = await request(apiPath("/export")); downloadJson(snapshot.export || {}, snapshot.filename || "forkmesh-stretch.json"); }
    catch (error) { message("error", `Could not export: ${error.message}`); }
  }
  async function uninstall() {
    if (!confirm("Uninstall Stretch Break? Your sessions download first, then they are deleted from web, desktop, and mobile.")) return;
    try {
      const snapshot = await request(apiPath("/export"));
      if ((snapshot.export?.sessions || []).length) downloadJson(snapshot.export, snapshot.filename || "forkmesh-stretch.json");
      await request(apiPath("/uninstall"), "POST", {}); state.installed = false; state.overview = null;
      $(`[data-stretch-manage-dialog]`).close(); message("notice", "Stretch Break uninstalled."); await load();
    } catch (error) { message("error", `Could not uninstall: ${error.message}`); }
  }
  async function init() {
    if (!currentSession()?.sessionToken) return message("error", "Sign in to use Stretch Break.");
    if (!speech) message("notice", "This browser cannot speak, so follow the text and the example person.");
    speech?.getVoices();
    await load();
    $(`[data-stretch-install]`)?.addEventListener("click", () => void install());
    $(`[data-stretch-refresh]`)?.addEventListener("click", () => void load());
    $(`[data-stretch-manage]`)?.addEventListener("click", () => $(`[data-stretch-manage-dialog]`).showModal());
    $(`[data-stretch-export]`)?.addEventListener("click", () => void exportMine());
    $(`[data-stretch-uninstall]`)?.addEventListener("click", () => void uninstall());
    $(`[data-stretch-voice]`)?.addEventListener("change", (event) => {
      state.voice = event.target.checked; if (!state.voice) hush(); void saveProfile({ voice: state.voice });
    });
    $(`[data-stretch-routines]`)?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-stretch-routine]"); if (!button) return;
      state.routine = button.dataset.stretchRoutine; renderPicker(); void saveProfile({ routine: state.routine });
    });
    $(`[data-stretch-start]`)?.addEventListener("click", startSession);
    $(`[data-stretch-pause]`)?.addEventListener("click", togglePause);
    $(`[data-stretch-back]`)?.addEventListener("click", back);
    $(`[data-stretch-skip]`)?.addEventListener("click", skip);
    $(`[data-stretch-stop]`)?.addEventListener("click", () => void finish(false));
    $(`[data-stretch-recent]`)?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-stretch-delete]"); if (button) void deleteSession(button.dataset.stretchDelete);
    });
    $$(`[data-stretch-dialog-close]`).forEach((button) => button.addEventListener("click", () => button.closest("dialog")?.close()));
    document.addEventListener("visibilitychange", () => { if (document.hidden) stopAnimation(); else startAnimation(); });
    window.addEventListener("pagehide", hush);
    document.addEventListener("keydown", (event) => {
      if (!state.run || event.target.closest?.("input, textarea, select")) return;
      if (event.key === " ") { event.preventDefault(); togglePause(); }
      else if (event.key === "ArrowRight") skip();
      else if (event.key === "ArrowLeft") back();
      else if (event.key === "Escape") void finish(false);
    });
    startAnimation();
    window.lucide?.createIcons?.();
  }
  return init();
});

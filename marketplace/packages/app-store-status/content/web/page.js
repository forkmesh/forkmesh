window.ForkMeshDashboard.registerPage("app-store-status", ({ select: $, session, escapeHtml }) => {
  const TONES = { action: "fm-label-danger", pending: "fm-label-attention", good: "fm-label-success", neutral: "" };
  const KINDS = [["downloads", "Downloads"], ["redownloads", "Redownloads"], ["updates", "Updates"]];
  const state = { loading: false, shots: [], index: 0 };
  const images = new Map();

  function apiPath(suffix = "") { return `/api/app-store-status${suffix}`; }
  async function request(path) {
    const headers = { accept: "application/json" };
    if (session()?.sessionToken) headers.authorization = `Bearer ${session().sessionToken}`;
    const response = await fetch(path, { headers, cache: "no-store" });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) throw new Error(String(payload.error || `HTTP ${response.status}`).replaceAll("_", " "));
    return payload;
  }
  function show(selector, visible) { $(selector)?.classList.toggle("hidden", !visible); }
  function message(value = "") {
    const node = $("[data-asc-error]"); if (!node) return;
    node.querySelector("[data-asc-message-text]").textContent = value;
    node.classList.toggle("hidden", !value);
  }
  function when(stamp) {
    const date = new Date(stamp || "");
    if (!stamp || Number.isNaN(date.getTime())) return "";
    return date.toLocaleString(undefined, { year: "numeric", month: "short", day: "numeric", hour: "numeric", minute: "2-digit" });
  }
  function ago(ms) {
    const seconds = Math.max(0, Math.round((Date.now() - Number(ms || 0)) / 1000));
    if (seconds < 60) return "just now";
    const steps = [[60, "minute"], [3600, "hour"], [86400, "day"]];
    const [size, unit] = seconds < 3600 ? steps[0] : seconds < 86400 ? steps[1] : steps[2];
    const count = Math.floor(seconds / size);
    return `${count} ${unit}${count === 1 ? "" : "s"} ago`;
  }
  function pill(status) {
    const value = status || {};
    return `<span class="fm-label ${TONES[value.tone] || ""}" title="${escapeHtml(value.state || "")}">${escapeHtml(value.label || "Unknown")}</span>`;
  }
  function row(title, pills, detail = "") {
    return `<div class="flex flex-wrap items-center justify-between gap-2 px-4 py-2.5"><div class="min-w-0"><p class="truncate text-sm font-medium text-foreground">${title}</p>${detail ? `<p class="mt-0.5 text-xs text-muted-foreground">${detail}</p>` : ""}</div><div class="flex flex-wrap items-center gap-1.5">${pills}</div></div>`;
  }
  function empty(text) { return `<p class="px-4 py-3 text-sm text-muted-foreground">${escapeHtml(text)}</p>`; }
  function fill(selector, rows, snapshot, section, none) {
    const problem = (snapshot.unavailable || {})[section];
    const notice = problem ? `<div class="px-4 py-2.5"><span class="fm-label fm-label-attention">Unavailable</span> <span class="text-xs text-muted-foreground">${escapeHtml(problem)}</span></div>` : "";
    $(selector).innerHTML = notice + (rows.length ? rows.join("") : problem ? "" : empty(none));
  }
  function shotPath(id) { return apiPath(`/screenshots/${encodeURIComponent(id)}`); }
  function picture(id) {
    if (!images.has(id)) {
      images.set(id, request(shotPath(id)).then((payload) => {
        if (!/^image\/(jpeg|png)$/.test(payload.mime || "") || !payload.data) throw new Error("not a picture");
        return `data:${payload.mime};base64,${payload.data}`;
      }).catch((error) => { images.delete(id); throw error; }));
    }
    return images.get(id);
  }
  function thumbnails(item, index) {
    const shots = item.screenshots || [];
    if (!shots.length) return "";
    return `<div class="mt-2 flex flex-wrap gap-2">${shots.map((shot, number) => `<button type="button" data-asc-shot="${escapeHtml(shot.id)}" data-asc-feedback-index="${index}" data-asc-shot-number="${number}" aria-label="Open screenshot ${number + 1} of ${shots.length} full size" class="grid h-24 w-16 place-items-center overflow-hidden rounded-md border border-border bg-background text-xs text-muted-foreground hover:border-accent focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent"><span data-asc-shot-placeholder>…</span><img alt="" class="hidden h-full w-full object-cover" /></button>`).join("")}</div>`;
  }
  function fillThumbnails() {
    document.querySelectorAll("[data-asc-shot]").forEach((button) => {
      const id = button.dataset.ascShot;
      picture(id).then((source) => {
        const image = button.querySelector("img");
        image.src = source;
        image.classList.remove("hidden");
        button.querySelector("[data-asc-shot-placeholder]")?.remove();
      }).catch(() => {
        const placeholder = button.querySelector("[data-asc-shot-placeholder]");
        if (placeholder) placeholder.textContent = "Not uploaded";
        button.title = "This screenshot has not been uploaded yet. Run the iOS App Store status shortcut again.";
      });
    });
  }
  function showShot() {
    const dialog = $("[data-asc-shot-dialog]");
    const entry = state.shots[state.index];
    if (!dialog || !entry) return;
    const image = $("[data-asc-shot-image]");
    const status = $("[data-asc-shot-status]");
    const save = $("[data-asc-shot-save]");
    $("[data-asc-shot-title]").textContent = state.shots.length > 1 ? `Screenshot ${state.index + 1} of ${state.shots.length}` : "Screenshot";
    $("[data-asc-shot-detail]").textContent = [entry.item.from, entry.item.device, when(entry.item.createdAt)].filter(Boolean).join(" · ");
    $("[data-asc-shot-prev]").disabled = state.index === 0;
    $("[data-asc-shot-next]").disabled = state.index >= state.shots.length - 1;
    show("[data-asc-shot-prev]", state.shots.length > 1);
    show("[data-asc-shot-next]", state.shots.length > 1);
    image.classList.add("hidden");
    save.classList.add("hidden");
    status.textContent = "Loading…";
    status.classList.remove("hidden");
    const wanted = entry.shot.id;
    picture(wanted).then((source) => {
      if (state.shots[state.index]?.shot.id !== wanted) return;
      image.src = source;
      image.alt = `Tester screenshot ${state.index + 1}${entry.item.comment ? `: ${entry.item.comment}` : ""}`;
      image.classList.remove("hidden");
      status.classList.add("hidden");
      save.href = source;
      save.download = `testflight-screenshot-${wanted.slice(0, 8)}.${source.startsWith("data:image/png") ? "png" : "jpg"}`;
      save.classList.remove("hidden");
    }).catch((error) => {
      if (state.shots[state.index]?.shot.id !== wanted) return;
      status.textContent = `This screenshot could not be loaded: ${error.message}. Run the iOS App Store status shortcut again to upload it.`;
    });
    if (!dialog.open) dialog.showModal();
  }
  function openShot(feedback, button) {
    const item = feedback[Number(button.dataset.ascFeedbackIndex)] || {};
    state.shots = (item.screenshots || []).map((shot) => ({ shot, item }));
    state.index = Math.min(Number(button.dataset.ascShotNumber) || 0, Math.max(0, state.shots.length - 1));
    showShot();
  }
  function plural(count, word) { return `${Number(count || 0).toLocaleString()} ${word}${count === 1 ? "" : "s"}`; }

  function renderDownloads(snapshot) {
    const found = snapshot.downloads || {};
    const problem = (snapshot.unavailable || {}).downloads;
    if (problem) return void ($("[data-asc-downloads]").innerHTML = empty(`Unavailable: ${problem}`));
    if (!found.available) return void ($("[data-asc-downloads]").innerHTML = empty(found.note || "No download figures yet."));
    const spans = [[`Since ${found.since}`, found.total || {}], ...(found.recent || []).slice().sort((a, b) => a.days - b.days).map((span) => [`Last ${span.days} days`, span])];
    $("[data-asc-downloads]").innerHTML = `<div class="grid divide-y divide-border sm:grid-cols-3 sm:divide-x sm:divide-y-0">${spans.map(([label, counts]) => `
      <div class="px-4 py-3"><p class="text-xs text-muted-foreground">${escapeHtml(label)}</p>
        <div class="mt-1 grid grid-cols-3 gap-2">${KINDS.map(([key, name]) => `<div><p class="text-lg font-semibold tabular-nums text-foreground">${Number(counts[key] || 0).toLocaleString()}</p><p class="text-xs text-muted-foreground">${name}</p></div>`).join("")}</div>
      </div>`).join("")}</div>`;
  }

  function render(payload) {
    const snapshot = payload.snapshot;
    show("[data-asc-loading]", false);
    show("[data-asc-empty]", !snapshot);
    show("[data-asc-app]", Boolean(snapshot));
    if (!snapshot) return;
    const app = snapshot.app || {};
    $("[data-asc-subtitle]").textContent = [app.bundleId || "iOS app", `published ${ago(payload.publishedAt)}`].join(" · ");
    $("[data-asc-subtitle]").title = payload.publishedAt ? new Date(payload.publishedAt).toLocaleString() : "";
    const attention = snapshot.attention || [];
    show("[data-asc-attention]", attention.length > 0);
    $("[data-asc-attention-list]").innerHTML = attention.map((line) => `<li>${escapeHtml(line)}</li>`).join("");

    fill("[data-asc-versions]", (snapshot.versions || []).map((item) => row(
      `${escapeHtml(item.version)} <span class="font-normal text-muted-foreground">${item.build ? `build ${escapeHtml(item.build)}` : "no build"}</span>`,
      pill(item.status),
      [item.createdAt ? `created ${escapeHtml(when(item.createdAt))}` : "", item.releaseType ? escapeHtml(item.releaseType.replaceAll("_", " ").toLowerCase()) : ""].filter(Boolean).join(" · "))),
    snapshot, "versions", "No App Store versions yet.");

    fill("[data-asc-submissions]", (snapshot.submissions || []).map((item) => row(
      item.submittedAt ? `Submitted ${escapeHtml(when(item.submittedAt))}` : "Not submitted",
      pill(item.status),
      (item.items || []).map((part) => `${escapeHtml(part.name)} ${pill(part.status)}`).join(" "))),
    snapshot, "submissions", "No App Review submissions.");

    renderDownloads(snapshot);

    fill("[data-asc-builds]", (snapshot.builds || []).map((item) => row(
      `${escapeHtml(item.version || "?")} <span class="font-normal text-muted-foreground">build ${escapeHtml(item.build)}</span>${item.expired ? ` <span class="fm-label">Expired</span>` : ""}`,
      pill(item.processing),
      [`<span class="inline-flex flex-wrap items-center gap-1.5">Internal ${pill(item.internal)} External ${pill(item.external)} Beta review ${pill(item.betaReview)}</span>`,
        item.uploadedAt ? `uploaded ${escapeHtml(when(item.uploadedAt))}` : "",
        `groups: ${escapeHtml((item.groups || []).join(", ") || "none")}`].filter(Boolean).join("<br>"))),
    snapshot, "builds", "No TestFlight builds.");

    fill("[data-asc-groups]", (snapshot.groups || []).map((item) => row(
      escapeHtml(item.name),
      `<span class="fm-label ${item.internal ? "" : "fm-label-accent"}">${item.internal ? "Internal" : "External"}</span>`,
      [plural(item.testers, "tester"), item.allBuilds ? "every build" : "chosen builds",
        item.internal ? "" : item.publicLinkEnabled && item.publicLink
          ? `<a class="text-accent hover:underline" href="${escapeHtml(item.publicLink)}" target="_blank" rel="noopener noreferrer">public link</a>${item.publicLinkLimit ? ` (limit ${Number(item.publicLinkLimit).toLocaleString()})` : ""}`
          : "public link off",
        item.feedbackEnabled ? "feedback on" : "feedback off"].filter(Boolean).join(" · "))),
    snapshot, "groups", "No TestFlight groups.");

    const feedback = snapshot.feedback || [];
    fill("[data-asc-feedback]", feedback.map((item, index) => `<div class="px-4 py-2.5">
      <p class="text-xs text-muted-foreground"><span class="fm-label ${item.kind === "crash" ? "fm-label-danger" : ""}">${item.kind === "crash" ? "Crash" : "Screenshot"}</span> ${escapeHtml(item.from)} · ${escapeHtml(item.device || "unknown device")} · ${escapeHtml(when(item.createdAt))}</p>
      <p class="mt-1 text-sm text-foreground">${escapeHtml(item.comment || "(no comment)")}</p>${thumbnails(item, index)}</div>`),
    snapshot, "feedback", "No tester feedback.");
    state.feedback = feedback;
    fillThumbnails();

    fill("[data-asc-reviews]", (snapshot.reviews || []).map((item) => {
      const rating = Math.max(0, Math.min(5, Number(item.rating || 0)));
      return `<div class="px-4 py-2.5">
        <p class="text-sm"><span class="text-attention" aria-label="${rating} of 5 stars">${"★".repeat(rating)}</span><span class="text-muted-foreground">${"★".repeat(5 - rating)}</span> <span class="font-medium text-foreground">${escapeHtml(item.title)}</span></p>
        <p class="mt-0.5 text-xs text-muted-foreground">${escapeHtml(item.territory)} · ${escapeHtml(when(item.createdAt))}</p>
        <p class="mt-1 text-sm text-foreground">${escapeHtml(item.body)}</p></div>`;
    }), snapshot, "reviews", "No customer reviews.");
  }

  async function load() {
    if (state.loading) return;
    state.loading = true;
    message();
    try {
      render(await request(apiPath()));
    } catch (error) {
      show("[data-asc-loading]", false);
      message(`Could not load App Store Status: ${error.message}`);
    } finally {
      state.loading = false;
    }
  }

  $("[data-asc-refresh]")?.addEventListener("click", load);
  $("[data-asc-feedback]")?.addEventListener("click", (event) => {
    const button = event.target.closest("[data-asc-shot]");
    if (button) openShot(state.feedback || [], button);
  });
  $("[data-asc-shot-close]")?.addEventListener("click", () => $("[data-asc-shot-dialog]")?.close());
  $("[data-asc-shot-prev]")?.addEventListener("click", () => { state.index = Math.max(0, state.index - 1); showShot(); });
  $("[data-asc-shot-next]")?.addEventListener("click", () => { state.index = Math.min(state.shots.length - 1, state.index + 1); showShot(); });
  $("[data-asc-shot-dialog]")?.addEventListener("keydown", (event) => {
    if (event.key === "ArrowLeft" && state.index > 0) { state.index -= 1; showShot(); }
    if (event.key === "ArrowRight" && state.index < state.shots.length - 1) { state.index += 1; showShot(); }
  });
  $("[data-asc-shot-dialog]")?.addEventListener("click", (event) => {
    if (event.target === event.currentTarget) event.currentTarget.close();
  });
  load();
});

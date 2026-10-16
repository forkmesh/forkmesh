// 09-achievements.js, loaded by dashboard.js when its page runs (app/src/dashboard_bundle.py LAZY_CHUNKS).
(() => {
  const chunks = window.__forkmeshDashboardChunks = window.__forkmeshDashboardChunks || {};
  chunks["achievements"] = (core) => {
  const { $, fetchJson, formatCount, formatDate, profileBadgeMedal, profileBadgeTier, state } = core;
  function achievementsAccountName() {
    const session = state.session;
    if (!session?.sessionToken || !session?.nodeName) return "";
    return String(session.nodeName).toLowerCase();
  }

  function achievementsTierLabel(tier) {
    const text = String(tier || "");
    return text ? text.charAt(0).toUpperCase() + text.slice(1) : "";
  }

  function achievementsShare(item) {
    const target = Number(item.nextThreshold) || 0;
    return target > 0 ? Math.min(1, (Number(item.progress) || 0) / target) : 0;
  }

  function achievementsEntries(catalog, mine, progress) {
    const earned = new Map((mine || []).map((badge) => [badge.slug, badge]));
    const pending = new Map((progress || []).map((step) => [step.slug, step]));
    const out = { earned: [], locked: [] };
    (catalog || []).forEach((badge, order) => {
      const own = earned.get(badge.slug);
      if (own) {
        out.earned.push({ ...badge, ...own, order });
        return;
      }
      if (badge.hiddenUntilEarned) return;
      const step = pending.get(badge.slug);
      const first = Array.isArray(badge.tiers) && badge.tiers.length ? Number(badge.tiers[0].threshold) || 0 : 0;
      out.locked.push({
        ...badge,
        order,
        progress: Math.max(0, Number(step?.progress) || 0),
        nextThreshold: Number(step?.nextThreshold) || first,
        unit: String(step?.unit || badge.unit || ""),
      });
    });
    out.earned.sort((a, b) => (Number(b.awardedAt) || 0) - (Number(a.awardedAt) || 0) || a.order - b.order);
    out.locked.sort((a, b) => achievementsShare(b) - achievementsShare(a) || a.order - b.order);
    return out;
  }

  function achievementsProgress(value, target, unit, label) {
    const wrap = document.createElement("div");
    wrap.className = "mt-2";
    const track = document.createElement("div");
    track.className = "h-1 w-full overflow-hidden rounded-full bg-subtle";
    track.setAttribute("role", "progressbar");
    track.setAttribute("aria-valuemin", "0");
    track.setAttribute("aria-valuemax", String(target));
    track.setAttribute("aria-valuenow", String(Math.min(value, target)));
    track.setAttribute("aria-label", label);
    const fill = document.createElement("div");
    fill.className = "h-1 rounded-full bg-primary";
    fill.style.width = `${Math.round(Math.min(1, target > 0 ? value / target : 0) * 100)}%`;
    track.append(fill);
    const text = document.createElement("p");
    text.className = "mt-1 text-xs leading-4 text-muted-foreground tabular-nums";
    text.textContent = `${formatCount(value)} of ${formatCount(target)}${unit ? " " + unit : ""} · ${label}`;
    wrap.append(track, text);
    return wrap;
  }

  function achievementsTile(item, locked) {
    const tile = document.createElement("li");
    tile.className = "flex items-start gap-3";
    tile.dataset.achievement = item.slug;
    if (locked) tile.dataset.achievementLocked = "";
    const art = document.createElement("span");
    art.className = "relative shrink-0";
    const medal = profileBadgeMedal(locked ? { slug: item.slug, name: item.name } : item);
    medal.classList.remove("h-12", "w-12");
    medal.classList.add("h-16", "w-16");
    if (medal instanceof HTMLImageElement) {
      medal.width = 64;
      medal.height = 64;
    }
    if (locked) {
      medal.classList.add("opacity-40");
      medal.style.filter = "grayscale(1)";
      const lock = document.createElement("span");
      lock.className = "absolute bottom-0 right-0 flex h-5 w-5 items-center justify-center rounded-full border border-border bg-card text-muted-foreground";
      lock.innerHTML = '<i data-lucide="lock" class="h-3 w-3"></i>';
      art.append(medal, lock);
    } else {
      art.append(medal);
    }
    const text = document.createElement("div");
    text.className = "min-w-0 flex-1";
    const title = document.createElement("p");
    title.className = "flex items-center gap-2 font-semibold leading-5 text-foreground";
    const name = document.createElement("span");
    name.className = "truncate";
    name.textContent = String(item.name || item.slug || "");
    title.append(name);
    const tier = locked ? "" : profileBadgeTier(item);
    if (tier) {
      const chip = document.createElement("span");
      chip.className = "shrink-0 rounded-full border border-border px-1.5 text-xs font-medium leading-4 text-muted-foreground";
      chip.textContent = achievementsTierLabel(tier);
      title.append(chip);
    }
    const detail = document.createElement("p");
    detail.className = "mt-0.5 text-xs leading-5 text-muted-foreground";
    detail.textContent = String(item.description || "");
    text.append(title, detail);
    const value = Math.max(0, Number(item.progress) || 0);
    const target = Number(item.nextThreshold) || 0;
    if (locked && target > 0 && value > 0) {
      text.append(achievementsProgress(value, target, item.unit, "First level"));
    } else if (locked && target > 0) {
      const first = document.createElement("p");
      first.className = "mt-2 text-xs leading-4 text-muted-foreground tabular-nums";
      first.textContent = `First level at ${formatCount(target)}${item.unit ? " " + item.unit : ""}`;
      text.append(first);
    } else if (!locked && item.nextTier && target > 0) {
      text.append(achievementsProgress(value, target, item.unit, `${achievementsTierLabel(item.nextTier)} next`));
    } else if (!locked && Array.isArray(item.tiers) && item.tier) {
      const top = document.createElement("p");
      top.className = "mt-2 text-xs leading-4 text-muted-foreground";
      top.textContent = "Highest level reached";
      text.append(top);
    }
    if (!locked && Number(item.awardedAt) > 0) {
      const when = document.createElement("p");
      when.className = "mt-1 text-xs leading-4 text-muted-foreground";
      when.textContent = "Earned " + formatDate(Number(item.awardedAt));
      text.append(when);
    }
    tile.append(art, text);
    return tile;
  }

  function renderAchievementsNotice(message) {
    const root = $("[data-achievements-notice]");
    if (!root) return;
    root.textContent = "";
    if (!message) return;
    const line = document.createElement("p");
    line.className = "px-4 sm:px-5 py-3 border-b border-border text-sm text-muted-foreground";
    line.textContent = message;
    root.append(line);
  }

  function renderAchievements(entries, signedIn) {
    const earnedList = $("[data-achievements-earned]");
    const lockedList = $("[data-achievements-locked]");
    const earnedSection = $("[data-achievements-earned-section]");
    const count = $("[data-achievements-count]");
    if (!earnedList || !lockedList) return;
    earnedList.textContent = "";
    lockedList.textContent = "";
    entries.earned.forEach((item) => earnedList.append(achievementsTile(item, false)));
    entries.locked.forEach((item) => lockedList.append(achievementsTile(item, true)));
    if (!entries.locked.length) {
      const done = document.createElement("li");
      done.className = "text-sm text-muted-foreground";
      done.textContent = "You have earned every badge there is.";
      lockedList.append(done);
    }
    if (earnedSection) earnedSection.hidden = !entries.earned.length;
    const total = entries.earned.length + entries.locked.length;
    if (count) count.textContent = signedIn ? `${entries.earned.length} of ${total} earned` : `${total} badges`;
    window.lucide?.createIcons();
  }

  async function loadAchievements() {
    const name = achievementsAccountName();
    try {
      const [catalogBody, mineBody] = await Promise.all([
        fetchJson("/api/badges"),
        name ? fetchJson("/api/badges/" + encodeURIComponent(name), { fresh: true }) : Promise.resolve(null),
      ]);
      const catalog = Array.isArray(catalogBody?.badges) ? catalogBody.badges : [];
      const mine = Array.isArray(mineBody?.badges) ? mineBody.badges : [];
      const progress = Array.isArray(mineBody?.progress) ? mineBody.progress : [];
      renderAchievementsNotice(name ? "" : "Log in to see the badges you have earned and how close you are to the rest.");
      renderAchievements(achievementsEntries(catalog, mine, progress), Boolean(name));
    } catch (_) {
      renderAchievementsNotice("Badges could not be loaded. Refresh the page to try again.");
      const count = $("[data-achievements-count]");
      if (count) count.textContent = "Unavailable";
    }
  }

  function initAchievementsPage() {
    if (!$("[data-achievements-panel]")) return;
    void loadAchievements();
  }
  return { initAchievementsPage };
  };
})();

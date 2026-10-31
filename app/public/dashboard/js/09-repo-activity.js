  const ACTIVITY_VIEWS = [
    { id: "pulse", label: "Pulse", icon: "activity" },
    { id: "leaderboard", label: "Leaderboard", icon: "trophy" },
    { id: "contributors", label: "Contributors", icon: "users" },
    { id: "frequency", label: "Code frequency", icon: "chart-column" },
    { id: "commits", label: "Recent commits", icon: "git-commit-horizontal" },
  ];
  const ACTIVITY_PERIODS = [
    { id: "24h", label: "1 day", phrase: "in the last day", short: "today" },
    { id: "3d", label: "3 days", phrase: "in the last 3 days", short: "these 3 days" },
    { id: "1w", label: "1 week", phrase: "in the last week", short: "this week" },
    { id: "1m", label: "1 month", phrase: "in the last month", short: "this month" },
  ];
  const ACTIVITY_DEFAULT_PERIOD = "1w";
  const ACTIVITY_LIST_PREVIEW = 10;
  const ACTIVITY_CHART_COLUMNS = 12;
  const ACTIVITY_MILESTONES = [
    { id: "first-merge", badge: "coder", label: "First merged pull request", metric: "mergedPulls", one: "merged pull request", unit: "merged pull requests", thresholds: [1] },
    { id: "merged", badge: "web_weaver", label: "Merged pull requests", metric: "mergedPulls", one: "merged pull request", unit: "merged pull requests" },
    { id: "reviews", badge: "six_eyes", label: "Reviewer", metric: "reviews", one: "review", unit: "reviews" },
    { id: "bugs", badge: "bug_catcher", label: "Bug squasher", metric: "bugsClosed", one: "bug issue closed", unit: "bug issues closed" },
    { id: "streak", badge: "daily_spinner", label: "Streak", metric: "longestStreak", one: "day in a row", unit: "days in a row" },
  ];
  const ACTIVITY_SCORE_PARTS = [
    { key: "merged", one: "merged pull request", label: "merged pull requests", short: ["merged PR", "merged PRs"], color: "var(--contribution-pulls)" },
    { key: "commits", one: "commit to the default branch", label: "commits to the default branch", short: ["commit", "commits"], color: "var(--contribution-commits)" },
    { key: "reviews", one: "review of someone else's pull request", label: "reviews", short: ["review", "reviews"], color: "var(--contribution-reviews)" },
    { key: "issuesClosed", one: "issue closed", label: "issues closed", short: ["closed issue", "closed issues"], color: "var(--contribution-issues)" },
    { key: "comments", one: "comment", label: "comments", short: ["comment", "comments"], color: "rgb(var(--dashboard-muted-foreground-rgb))" },
    { key: "pullsOpened", one: "pull request opened", label: "pull requests opened", short: ["opened PR", "opened PRs"], color: "var(--dashboard-link)" },
    { key: "issuesOpened", one: "issue opened", label: "issues opened", short: ["opened issue", "opened issues"], color: "var(--dashboard-open)" },
  ];
  const ACTIVITY_TIE_LABELS = { merged: "merged pull requests", reviews: "reviews", issuesClosed: "closed issues", activeDays: "active days", commits: "commits", pullsOpened: "pull requests opened", issuesOpened: "issues opened", comments: "comments" };
  const ACTIVITY_STYLE_ID = "fm-activity-style";
  const ACTIVITY_STYLE = `
.fm-activity-nav a{display:flex;align-items:center;gap:8px;height:32px;padding:0 10px;border-radius:6px;font-size:14px;color:rgb(var(--dashboard-muted-foreground-rgb));white-space:nowrap}
.fm-activity-nav a:hover{background:rgb(var(--dashboard-secondary-rgb));color:rgb(var(--dashboard-foreground-rgb))}
.fm-activity-nav a[aria-current="page"]{background:rgb(var(--dashboard-secondary-rgb));color:var(--dashboard-link);box-shadow:inset 2px 0 0 var(--dashboard-link);font-weight:600}
.fm-activity-nav svg,.fm-activity-nav i{width:16px;height:16px;flex:none}
@media (min-width:768px){.fm-activity-nav ul{display:grid;gap:2px}}
.fm-label.fm-activity-done{border-color:var(--contribution-pulls);color:var(--contribution-pulls)}
.fm-activity-done-text{color:var(--contribution-pulls)}
.fm-activity-open-text{color:var(--dashboard-open)}
.fm-activity-meta{flex-basis:100%}
@media (min-width:640px){.fm-activity-meta{flex-basis:auto}}
.fm-activity-swatch{display:inline-block;width:8px;height:8px;flex:none;border-radius:999px}
.fm-activity-ratio{display:flex;height:8px;overflow:hidden;border-radius:999px;background:rgb(var(--dashboard-secondary-rgb))}
.fm-activity-ratio>span{display:block;height:100%}
.fm-activity-ratio>span+span{margin-left:2px}
.fm-activity-chart{display:flex;align-items:flex-end;justify-content:center;gap:8px;height:184px}
.fm-activity-column{display:flex;flex:1 1 0;min-width:0;max-width:56px;height:100%;flex-direction:column;align-items:center;gap:6px}
.fm-activity-track{display:flex;flex:1 1 auto;width:100%;align-items:flex-end;justify-content:center}
.fm-activity-fill{display:block;width:70%;max-width:28px;min-height:2px;border-radius:3px 3px 0 0;background:var(--contribution-commits)}
@media (max-width:639px){.fm-activity-chart>.fm-activity-column:nth-child(n+8){display:none}}
.fm-activity-row{display:grid;grid-template-columns:2rem minmax(0,1fr) auto;align-items:center;gap:12px}
.fm-activity-row .fm-activity-move-wide,.fm-activity-row .fm-activity-medals{display:none}
@media (min-width:640px){.fm-activity-row{grid-template-columns:2rem 3.5rem minmax(0,1fr) auto 5rem}.fm-activity-row .fm-activity-move-wide,.fm-activity-row .fm-activity-medals{display:flex}.fm-activity-row .fm-activity-move-narrow{display:none}}
.fm-activity-rank{display:inline-flex;width:28px;height:28px;align-items:center;justify-content:center;border-radius:999px;font-size:13px;font-weight:600;font-variant-numeric:tabular-nums;color:rgb(var(--dashboard-foreground-rgb))}
.fm-activity-rank[data-podium="1"]{box-shadow:inset 0 0 0 2px #d4a72c}
.fm-activity-rank[data-podium="2"]{box-shadow:inset 0 0 0 2px #8c959f}
.fm-activity-rank[data-podium="3"]{box-shadow:inset 0 0 0 2px #b4693e}
.fm-activity-podium{display:grid;gap:12px;padding:16px;border-bottom:1px solid rgb(var(--dashboard-border-rgb))}
@media (min-width:640px){.fm-activity-podium{grid-template-columns:repeat(3,minmax(0,1fr));align-items:end}.fm-activity-podium-card[data-podium="1"]{order:2;padding-top:22px;padding-bottom:22px}.fm-activity-podium-card[data-podium="2"]{order:1}.fm-activity-podium-card[data-podium="3"]{order:3}}
.fm-activity-podium-card{display:flex;min-width:0;flex-direction:column;align-items:center;gap:6px;padding:14px 12px;border-radius:10px;border:1px solid rgb(var(--dashboard-border-rgb));background:rgb(var(--dashboard-subtle-rgb));text-align:center}
.fm-activity-podium-card[data-podium="1"]{border-color:#d4a72c;box-shadow:0 0 0 1px #d4a72c33}
.fm-activity-podium-card[data-podium="2"]{border-color:#8c959f}
.fm-activity-podium-card[data-podium="3"]{border-color:#b4693e}
.fm-activity-podium-score{font-size:22px;font-weight:700;line-height:1;font-variant-numeric:tabular-nums;color:rgb(var(--dashboard-foreground-rgb))}
.fm-activity-podium-card .fm-activity-rank{width:26px;height:26px}
.fm-activity-podium-avatar{display:inline-flex}
.fm-activity-podium-avatar>*{width:40px!important;height:40px!important;font-size:15px}
.fm-activity-parts{display:flex;min-width:0;flex-wrap:wrap;align-items:center;gap:2px 10px;font-size:12px;color:rgb(var(--dashboard-muted-foreground-rgb))}
.fm-activity-parts>span{display:inline-flex;align-items:center;gap:4px;white-space:nowrap}
.fm-activity-podium-card .fm-activity-parts{justify-content:center}
.fm-activity-tip-wrap{position:relative;display:inline-flex}
.fm-activity-tip{position:absolute;right:0;bottom:calc(100% + 6px);z-index:30;display:none;width:max-content;max-width:min(18rem,80vw);text-align:left;white-space:normal}
.fm-activity-tip-wrap:hover .fm-activity-tip,.fm-activity-tip-wrap:focus-within .fm-activity-tip{display:block}
.fm-activity-score{display:inline-flex;align-items:center;gap:6px;min-width:3.5rem;justify-content:flex-end;padding:2px 6px;border-radius:6px;font-weight:600;font-variant-numeric:tabular-nums;color:rgb(var(--dashboard-foreground-rgb))}
.fm-activity-score:hover{background:rgb(var(--dashboard-secondary-rgb))}
.fm-activity-skeleton{display:block;border-radius:6px;background:rgb(var(--dashboard-secondary-rgb));animation:fm-activity-wait 1.4s ease-in-out infinite}
@keyframes fm-activity-wait{50%{opacity:.5}}
.fm-activity-weeks{display:flex;align-items:flex-end;gap:2px;height:128px}
.fm-activity-weeks>span{flex:1 1 0;min-width:2px;border-radius:2px 2px 0 0;background:var(--contribution-commits)}
.fm-activity-diff{display:grid;grid-auto-flow:column;grid-auto-columns:minmax(0,1fr);gap:3px;height:176px}
.fm-activity-diff>span{display:grid;grid-template-rows:1fr 1fr;row-gap:1px}
.fm-activity-diff>span>span{display:flex;justify-content:center}
.fm-activity-diff>span>span:first-child{align-items:flex-end;border-bottom:1px solid rgb(var(--dashboard-border-rgb))}
.fm-activity-diff>span>span:last-child{align-items:flex-start}
.fm-activity-diff i{display:block;width:70%;max-width:24px}
.fm-activity-burst{position:absolute;left:50%;top:50%;width:0;height:0;pointer-events:none}
.fm-activity-burst i{position:absolute;left:-3px;top:-3px;width:6px;height:6px;border-radius:1px;background:hsl(var(--hue) 75% 58%);animation:fm-activity-burst .9s ease-out var(--delay) forwards;opacity:0}
@keyframes fm-activity-burst{0%{opacity:1;transform:rotate(var(--angle)) translateY(0) scale(1)}100%{opacity:0;transform:rotate(var(--angle)) translateY(-38px) scale(.6)}}
@media (prefers-reduced-motion:reduce){.fm-activity-skeleton{animation:none}.fm-activity-burst{display:none}}
`;
  const activityView = {
    repoKey: "",
    view: "pulse",
    period: ACTIVITY_DEFAULT_PERIOD,
    scope: "period",
    pulse: new Map(),
    person: new Map(),
    insights: null,
    history: null,
    catalog: null,
    expanded: new Set(),
    seq: 0,
    bound: null,
  };

  function activityEnsureStyle() {
    if (document.getElementById(ACTIVITY_STYLE_ID)) return;
    const style = document.createElement("style");
    style.id = ACTIVITY_STYLE_ID;
    style.textContent = ACTIVITY_STYLE;
    document.head.append(style);
  }

  function activityPeriod(id) {
    return ACTIVITY_PERIODS.find((item) => item.id === id) || ACTIVITY_PERIODS.find((item) => item.id === ACTIVITY_DEFAULT_PERIOD);
  }

  function activityReadLocation() {
    const params = new URLSearchParams(location.search || "");
    const view = params.get("view") || "";
    const period = params.get("period") || "";
    return {
      view: ACTIVITY_VIEWS.some((item) => item.id === view) ? view : "pulse",
      period: ACTIVITY_PERIODS.some((item) => item.id === period) ? period : ACTIVITY_DEFAULT_PERIOD,
      scope: params.get("scope") === "all" ? "all" : "period",
    };
  }

  function activityHref(repo, view, period, scope) {
    const params = new URLSearchParams();
    if (view && view !== "pulse") params.set("view", view);
    if (period && period !== ACTIVITY_DEFAULT_PERIOD) params.set("period", period);
    if (view === "leaderboard" && scope === "all") params.set("scope", "all");
    const query = params.toString();
    return `${repoLinkUrl(repo)}/insights${query ? `?${query}` : ""}`;
  }

  function activityRememberLocation(repo) {
    if (!/\/insights\/?$/.test(location.pathname)) return;
    const next = activityHref(repo, activityView.view, activityView.period, activityView.scope);
    if (`${location.pathname}${location.search}` !== next) window.history.replaceState(window.history.state, "", next);
  }

  function activityWhen(ms) {
    const value = Number(ms) || 0;
    return value ? relativeTimeLabel(value) : "";
  }

  function activityRange(data) {
    const options = { month: "short", day: "numeric", year: "numeric" };
    const since = data?.since ? new Date(data.since) : null;
    const until = data?.until ? new Date(data.until) : null;
    if (!until || Number.isNaN(until.getTime())) return "";
    const end = until.toLocaleDateString(undefined, options);
    if (!since || Number.isNaN(since.getTime())) return `All time to ${end}`;
    return `${since.toLocaleDateString(undefined, options)} – ${end}`;
  }

  function activityPlural(count, singular, plural) {
    return `${formatCount(count)} ${Number(count) === 1 ? singular : plural || `${singular}s`}`;
  }

  function activityNameHtml(name, extra = "") {
    const text = String(name || "").trim() || "unknown";
    return `<span data-repo-insights-contributor="${escapeHtml(text)}" class="min-w-0 truncate">${issuePersonNameHtml(text, extra || "font-medium text-foreground", { git: true })}</span>`;
  }

  function activityAvatarHtml(person) {
    return repoContributorAvatar({ name: person?.name || "", avatarUrl: person?.avatarUrl || "", commits: Number(person?.commits) || 0 });
  }

  function activityTiers(milestone) {
    if (Array.isArray(milestone.thresholds)) {
      return milestone.thresholds.map((threshold) => ({ tier: "", threshold }));
    }
    const entry = (activityView.catalog || []).find((badge) => badge.slug === milestone.badge);
    return (Array.isArray(entry?.tiers) ? entry.tiers : [])
      .map((item) => ({ tier: String(item.tier || ""), threshold: Number(item.threshold) || 0 }))
      .filter((item) => item.threshold > 0)
      .sort((a, b) => a.threshold - b.threshold);
  }

  function activityLevel(tiers, value) {
    let reached = -1;
    tiers.forEach((item, index) => {
      if (Number(value) >= item.threshold) reached = index;
    });
    return reached;
  }

  function activityTierName(tier) {
    const text = String(tier || "");
    return text && text !== "default" ? text.charAt(0).toUpperCase() + text.slice(1) : "";
  }

  function activityMilestoneTitle(milestone, tier) {
    const name = activityTierName(tier);
    return name ? `${milestone.label} · ${name}` : milestone.label;
  }

  function activityMedalHtml(milestone, tier, size, label) {
    return `<span data-activity-medal="${escapeHtml(milestone.badge)}" data-activity-tier="${escapeHtml(tier || "")}" data-activity-size="${size}" title="${escapeHtml(label)}" class="inline-flex shrink-0" style="width:${size}px;height:${size}px"><span class="sr-only">${escapeHtml(label)}</span></span>`;
  }

  function activityHydrateMedals(root) {
    root?.querySelectorAll("[data-activity-medal]").forEach((holder) => {
      if (holder.dataset.activityMedalReady === "1") return;
      holder.dataset.activityMedalReady = "1";
      const size = Number(holder.dataset.activitySize) || 20;
      const tier = holder.dataset.activityTier || "";
      const medal = profileBadgeMedal({ slug: holder.dataset.activityMedal || "", tier: tier === "default" ? "" : tier, name: holder.title });
      medal.classList.remove("h-12", "w-12");
      medal.style.width = `${size}px`;
      medal.style.height = `${size}px`;
      if (medal instanceof HTMLImageElement) {
        medal.width = size;
        medal.height = size;
      } else {
        medal.style.fontSize = `${Math.max(9, Math.round(size / 2.4))}px`;
        medal.classList.remove("border-2");
      }
      medal.setAttribute("aria-hidden", "true");
      holder.prepend(medal);
    });
  }

  function activityEarned(achievements) {
    const earned = [];
    ACTIVITY_MILESTONES.forEach((milestone) => {
      const tiers = activityTiers(milestone);
      const value = Number(achievements?.[milestone.metric]) || 0;
      const level = activityLevel(tiers, value);
      if (level < 0) return;
      if (milestone.id === "first-merge") {
        const merged = ACTIVITY_MILESTONES.find((item) => item.id === "merged");
        if (activityLevel(activityTiers(merged), value) >= 0) return;
      }
      earned.push({ milestone, tier: tiers[level].tier, value });
    });
    return earned;
  }

  function activitySegmented(attribute, options, current, label) {
    return `<div class="fm-segmented inline-flex" role="group" aria-label="${escapeHtml(label)}">${options.map((item) => `<button type="button" ${attribute}="${escapeHtml(item.id)}" aria-pressed="${item.id === current ? "true" : "false"}">${escapeHtml(item.label)}</button>`).join("")}</div>`;
  }

  function activityHeaderHtml(title, subtitle, controls) {
    return `<div class="flex flex-wrap items-start justify-between gap-3">
      <div class="min-w-0">
        <h2 class="text-xl font-semibold text-foreground">${escapeHtml(title)}</h2>
        <p data-activity-subtitle class="mt-1 text-sm text-muted-foreground">${subtitle}</p>
      </div>
      <div class="flex flex-wrap items-center gap-2">${controls}<button type="button" data-repo-insights-refresh aria-label="Refresh activity" title="Refresh" class="fm-btn fm-btn-sm fm-btn-icon"><i data-lucide="refresh-cw"></i></button></div>
    </div>`;
  }

  function activityShellHtml(repo) {
    const nav = ACTIVITY_VIEWS.map((item) => `<li><a href="${escapeHtml(activityHref(repo, item.id, activityView.period, activityView.scope))}" data-activity-view="${item.id}"${item.id === activityView.view ? ' aria-current="page"' : ""}><i data-lucide="${item.icon}" aria-hidden="true"></i><span>${escapeHtml(item.label)}</span></a></li>`).join("");
    return `<div data-repo-activity data-repo-rail-layout class="grid min-w-0 gap-5 py-2 md:grid-cols-[16rem_minmax(0,1fr)]">
      <nav data-repo-rail class="fm-activity-nav min-w-0" aria-label="Activity views"><div data-repo-rail-inner><h2 class="fm-rail-title hidden lg:block">Insights</h2><ul class="flex flex-wrap gap-1">${nav}</ul></div></nav>
      <div data-activity-main data-repo-rail-main class="grid min-w-0 content-start gap-5" aria-live="polite" aria-busy="true"></div>
    </div>`;
  }

  function activitySkeleton(heights) {
    return heights.map((height) => `<span class="fm-activity-skeleton" style="height:${height}px"></span>`).join("");
  }

  function activitySection(id, title, body, icon = "", extra = "") {
    return `<section ${id ? `id="${escapeHtml(id)}" ` : ""}class="min-w-0 overflow-hidden rounded-md border border-border"${extra}>
      <h3 class="flex items-center gap-2 border-b border-border bg-[rgb(var(--dashboard-subtle-rgb))] px-4 py-2 text-[14px] font-semibold text-foreground">${icon}${title}</h3>
      ${body}
    </section>`;
  }

  function activityRatioHtml(parts, label) {
    const total = parts.reduce((sum, part) => sum + part.value, 0);
    const spans = total ? parts.filter((part) => part.value > 0).map((part) => `<span style="width:${(part.value / total * 100).toFixed(2)}%;background:${part.color}"></span>`).join("") : "";
    return `<div class="fm-activity-ratio" role="img" aria-label="${escapeHtml(label)}">${spans}</div>`;
  }

  function activityOverviewHtml(data) {
    const pulls = data.pulls || {};
    const issues = data.issues || {};
    const merged = Number(pulls.merged?.count) || 0;
    const proposed = Number(pulls.opened?.count) || 0;
    const stillOpen = (pulls.opened?.items || []).filter((item) => item.status === "open").length;
    const closed = Number(issues.closed?.count) || 0;
    const opened = Number(issues.opened?.count) || 0;
    const newOpen = (issues.opened?.items || []).filter((item) => item.status === "open").length;
    const done = "var(--contribution-pulls)";
    const open = "var(--dashboard-open)";
    const half = (title, parts, label, legend) => `<div class="min-w-0 bg-background p-4">
        ${activityRatioHtml(parts, label)}
        <p class="mt-3 text-sm text-foreground">${title}</p>
        <p class="mt-1 flex flex-wrap gap-x-4 gap-y-1 text-xs text-muted-foreground">${legend}</p>
      </div>`;
    const dot = (color) => `<span class="fm-activity-swatch" style="background:${color}" aria-hidden="true"></span>`;
    const cell = (target, icon, tone, count, label) => `<a href="#${target}" data-activity-jump="${target}" class="flex min-w-0 flex-col items-center gap-1 bg-background px-3 py-4 text-center hover:bg-secondary/40">
        <span class="inline-flex items-center gap-1.5 text-2xl font-semibold text-foreground"><i data-lucide="${icon}" class="h-5 w-5 ${tone}" aria-hidden="true"></i>${formatCount(count)}</span>
        <span class="text-xs text-muted-foreground">${escapeHtml(label)}</span>
      </a>`;
    return activitySection("", "Overview", `
      <div class="grid gap-px bg-border sm:grid-cols-2">
        ${half(`<strong>${formatCount(merged + stillOpen)}</strong> active pull ${merged + stillOpen === 1 ? "request" : "requests"}`, [{ value: merged, color: done }, { value: stillOpen, color: open }], `${merged} merged and ${stillOpen} open pull requests`, `<span class="inline-flex items-center gap-1.5">${dot(done)}${formatCount(merged)} merged</span><span class="inline-flex items-center gap-1.5">${dot(open)}${formatCount(stillOpen)} open</span>`)}
        ${half(`<strong>${formatCount(closed + newOpen)}</strong> active ${closed + newOpen === 1 ? "issue" : "issues"}`, [{ value: closed, color: done }, { value: newOpen, color: open }], `${closed} closed and ${newOpen} new issues`, `<span class="inline-flex items-center gap-1.5">${dot(done)}${formatCount(closed)} closed</span><span class="inline-flex items-center gap-1.5">${dot(open)}${formatCount(newOpen)} new</span>`)}
      </div>
      <div class="grid grid-cols-2 gap-px border-t border-border bg-border sm:grid-cols-4">
        ${cell("activity-merged", "git-merge", "fm-activity-done-text", merged, merged === 1 ? "Merged pull request" : "Merged pull requests")}
        ${cell("activity-proposed", "git-pull-request", "fm-activity-open-text", proposed, proposed === 1 ? "Proposed pull request" : "Proposed pull requests")}
        ${cell("activity-closed", "circle-check", "fm-activity-done-text", closed, closed === 1 ? "Closed issue" : "Closed issues")}
        ${cell("activity-opened", "circle-dot", "fm-activity-open-text", opened, opened === 1 ? "New issue" : "New issues")}
      </div>`, "", ' data-activity-overview');
  }

  function activitySummaryHtml(data) {
    const summary = data.summary || {};
    const branch = escapeHtml(data.branch || "the default branch");
    const sentence = `Excluding merges and automated accounts, <strong>${activityPlural(summary.authors || 0, "author")}</strong> ${Number(summary.authors) === 1 ? "has" : "have"} pushed <strong>${activityPlural(summary.main || 0, "commit")}</strong> to <span class="font-mono">${branch}</span> and <strong>${activityPlural(summary.all || 0, "commit")}</strong> to all branches. On <span class="font-mono">${branch}</span>, <strong>${activityPlural(summary.filesChanged || 0, "file")}</strong> ${Number(summary.filesChanged) === 1 ? "has" : "have"} changed and there have been <strong class="text-success">${activityPlural(summary.additions || 0, "addition")}</strong> and <strong class="text-destructive">${activityPlural(summary.deletions || 0, "deletion")}</strong>.`;
    const notes = [];
    if (summary.partial) notes.push(`Line counts cover the ${formatCount(summary.measuredCommits || 0)} most recent commits so far; refresh in a moment for the rest.`);
    else if (summary.truncated) notes.push(`Line counts cover the ${formatCount(summary.measuredCommits || 0)} most recent commits on ${branch}.`);
    const committers = Array.isArray(data.committers) ? data.committers.slice(0, ACTIVITY_CHART_COLUMNS) : [];
    const max = Math.max(1, ...committers.map((row) => Number(row.commits) || 0));
    const chart = committers.length ? `<div class="border-t border-border px-4 pb-4 pt-4">
        <div class="fm-activity-chart" role="list" aria-label="Top committers on ${branch}">${committers.map((row) => {
          const count = Number(row.commits) || 0;
          const label = `${row.name}: ${activityPlural(count, "commit")}`;
          return `<div class="fm-activity-column" role="listitem" aria-label="${escapeHtml(label)}">
            <span class="font-mono text-[11px] text-muted-foreground" aria-hidden="true">${formatCount(count)}</span>
            <span class="fm-activity-track" aria-hidden="true"><span class="fm-activity-fill" style="height:${Math.max(2, Math.round(count / max * 100))}%"></span></span>
            <span data-activity-committer class="flex w-full min-w-0 flex-col items-center gap-1">${activityAvatarHtml(row)}<span class="w-full truncate text-center text-[11px]">${activityNameHtml(row.name, "text-muted-foreground")}</span></span>
          </div>`;
        }).join("")}</div>
        ${Number(data.committerCount) > committers.length ? `<p class="mt-3 text-center text-xs text-muted-foreground">Top ${formatCount(committers.length)} of ${activityPlural(data.committerCount, "committer")}</p>` : ""}
      </div>` : "";
    return `<section data-activity-commits class="min-w-0 overflow-hidden rounded-md border border-border">
      <p data-activity-sentence class="px-4 py-3 text-sm leading-6 text-foreground">${sentence}</p>
      ${notes.length ? `<p class="px-4 pb-3 text-xs text-muted-foreground">${notes.join(" ")}</p>` : ""}
      ${chart}
    </section>`;
  }

  function activityStateLabel(kind, status) {
    if (kind === "pulls" && (status === "merged" || status === "closed-merged")) return `<span class="fm-label fm-activity-done"><i data-lucide="git-merge" aria-hidden="true"></i><span>Merged</span></span>`;
    if (kind === "pulls" && status === "open") return `<span class="fm-label fm-label-success"><i data-lucide="git-pull-request" aria-hidden="true"></i><span>Open</span></span>`;
    if (kind === "pulls") return `<span class="fm-label fm-label-danger"><i data-lucide="git-pull-request-closed" aria-hidden="true"></i><span>Closed</span></span>`;
    if (status === "closed") return `<span class="fm-label fm-activity-done"><i data-lucide="circle-check" aria-hidden="true"></i><span>Closed</span></span>`;
    return `<span class="fm-label fm-label-success"><i data-lucide="circle-dot" aria-hidden="true"></i><span>Open</span></span>`;
  }

  function activityListHtml(repo, id, kind, listing, title, icon, empty) {
    const items = Array.isArray(listing?.items) ? listing.items : [];
    const expanded = activityView.expanded.has(id);
    const rows = items.map((item, index) => {
      const href = repoRecordUrl(repo, kind, Number(item.number));
      return `<li class="flex min-w-0 flex-wrap items-center gap-x-3 gap-y-1 px-4 py-2${index >= ACTIVITY_LIST_PREVIEW && !expanded ? " hidden" : ""}" data-activity-item>
        ${activityStateLabel(kind, item.status)}
        <a href="${escapeHtml(href)}" data-activity-record="${kind}" data-activity-number="${escapeHtml(item.number)}" class="min-w-0 flex-1 truncate text-[14px] text-foreground hover:text-accent hover:underline"><span class="font-mono text-muted-foreground">#${escapeHtml(item.number)}</span> ${escapeHtml(item.title || `${kind === "pulls" ? "Pull request" : "Issue"} #${item.number}`)}</a>
        <span class="fm-activity-meta flex min-w-0 shrink-0 items-center gap-1 text-xs text-muted-foreground">${item.author ? `${activityNameHtml(item.author, "max-w-[10rem] text-muted-foreground")}<span aria-hidden="true">·</span>` : ""}<time datetime="${escapeHtml(new Date(Number(item.at) || 0).toISOString())}">${escapeHtml(activityWhen(item.at))}</time></span>
      </li>`;
    }).join("");
    const more = items.length > ACTIVITY_LIST_PREVIEW
      ? `<button type="button" data-activity-more="${id}" aria-controls="${id}-list" aria-expanded="${expanded ? "true" : "false"}" class="flex w-full items-center justify-center gap-1.5 border-t border-border px-4 py-2 text-[13px] text-primary hover:bg-secondary/40"><span>${expanded ? "Show fewer" : `Show all ${formatCount(items.length)}`}</span><i data-lucide="chevron-down" class="h-4 w-4" aria-hidden="true"></i></button>`
      : "";
    const truncated = listing?.truncated ? `<p class="border-t border-border px-4 py-2 text-xs text-muted-foreground">Showing the ${formatCount(items.length)} most recent of ${formatCount(listing.count)}.</p>` : "";
    const body = items.length
      ? `<ul id="${id}-list" class="divide-y divide-border">${rows}</ul>${more}${truncated}`
      : `<p class="px-4 py-4 text-sm text-muted-foreground">${escapeHtml(empty)}</p>`;
    return activitySection(id, title, body, icon, ` data-activity-list="${id}"`);
  }

  function activityNewcomersHtml(data) {
    const people = Array.isArray(data.newcomers) ? data.newcomers : [];
    if (!people.length) return "";
    const what = (person) => {
      const number = Number(person.number) || 0;
      if (person.kind === "pull") return `opened pull request #${number}`;
      if (person.kind === "merged") return `merged pull request #${number}`;
      if (person.kind === "issue") return `opened issue #${number}`;
      if (person.kind === "review") return `reviewed pull request #${number}`;
      if (person.kind === "comment") return "left a first comment";
      if (person.kind === "closed") return `closed issue #${number}`;
      return "made a first commit";
    };
    const count = Number(data.newcomerCount) || people.length;
    return activitySection("activity-newcomers", `${activityPlural(count, "newcomer")} ${activityPeriod(activityView.period).phrase}`, `
      <p class="px-4 pt-3 text-sm text-muted-foreground">A warm welcome to the people who made their first contribution here.</p>
      <ul class="grid gap-3 p-4">${people.slice(0, 8).map((person) => `<li class="flex min-w-0 items-center gap-3">
        ${activityAvatarHtml(person)}
        <span class="min-w-0"><span class="flex min-w-0 items-center gap-2">${activityNameHtml(person.name)}<span class="fm-label fm-label-accent shrink-0"><i data-lucide="sparkles" aria-hidden="true"></i>First contribution</span></span>
        <span class="block truncate text-xs text-muted-foreground">${escapeHtml(what(person))} · ${escapeHtml(activityWhen(person.at))}</span></span>
      </li>`).join("")}</ul>
      ${count > 8 ? `<p class="border-t border-border px-4 py-2 text-xs text-muted-foreground">and ${activityPlural(count - 8, "other newcomer")}</p>` : ""}`, '<i data-lucide="sparkles" class="h-4 w-4 text-muted-foreground" aria-hidden="true"></i>', " data-activity-newcomers");
  }

  function activityProgressHtml(value, target, label) {
    const share = target > 0 ? Math.min(1, value / target) : 0;
    return `<div class="mt-2 h-1.5 w-full overflow-hidden rounded-full bg-secondary" role="progressbar" aria-label="${escapeHtml(label)}" aria-valuemin="0" aria-valuemax="${target}" aria-valuenow="${Math.min(value, target)}"><div class="h-full rounded-full bg-primary" style="width:${Math.round(share * 100)}%"></div></div>`;
  }

  function activityYouHtml(repo, person, scopeAll) {
    if (!person) return "";
    const period = activityPeriod(activityView.period);
    const totals = person.totals || {};
    const before = person.before || {};
    const when = scopeAll ? "all time" : period.short;
    if (!person.found) {
      return `<section data-activity-you class="rounded-md border border-border p-4">
        <p class="text-sm font-semibold text-foreground">Your activity</p>
        <p class="mt-1 text-sm text-muted-foreground">You have no recorded activity in this repository yet. Merged pull requests, reviews, closed issues and comments here all count toward the leaderboard. Bots, AI agents and mirror nodes aren't ranked.</p>
      </section>`;
    }
    const milestones = ACTIVITY_MILESTONES.map((milestone) => {
      const tiers = activityTiers(milestone);
      return { milestone, tiers, value: Number(totals[milestone.metric]) || 0, previous: Number(before[milestone.metric]) || 0 };
    }).filter((item) => item.tiers.length);
    const crossed = milestones.filter((item) => activityLevel(item.tiers, item.value) > activityLevel(item.tiers, item.previous)).map((item) => ({ ...item, tier: item.tiers[activityLevel(item.tiers, item.value)].tier }));
    const fresh = crossed.some((item) => item.milestone.id === "merged") ? crossed.filter((item) => item.milestone.id !== "first-merge") : crossed;
    const next = milestones.map((item) => {
      const target = item.tiers.find((tier) => tier.threshold > item.value);
      return target ? { ...item, target: target.threshold, tier: target.tier, share: item.value / target.threshold } : null;
    }).filter(Boolean).sort((a, b) => b.share - a.share)[0];
    const streak = Number(totals.currentStreak) || 0;
    const best = Number(totals.longestStreak) || 0;
    const stats = [
      person.rank ? `<span><strong class="text-foreground">#${formatCount(person.rank)}</strong> ${escapeHtml(when)}</span>` : `<span>Not ranked ${escapeHtml(when)}</span>`,
      `<span><strong class="text-foreground">${formatCount(person.score || 0)}</strong> ${Number(person.score) === 1 ? "point" : "points"}</span>`,
      `<span class="inline-flex items-center gap-1"><i data-lucide="flame" class="h-4 w-4 ${streak >= 3 ? "text-attention" : "text-muted-foreground"}" aria-hidden="true"></i>${streak ? `<strong class="text-foreground">${formatCount(streak)}-day</strong> streak` : "No current streak"}${best > streak ? ` · best ${formatCount(best)}` : ""}</span>`,
    ];
    const celebrations = fresh.map((item) => {
      const title = activityMilestoneTitle(item.milestone, item.tier);
      return `<li class="flex min-w-0 items-center gap-3">
        <span class="relative inline-flex" data-activity-celebrate="${escapeHtml(`${repoKey(repo)}|${item.milestone.id}|${item.tier}`)}">${activityMedalHtml(item.milestone, item.tier, 40, title)}</span>
        <span class="min-w-0 text-sm text-foreground"><strong>New ${escapeHtml(scopeAll ? "this week" : period.short)}:</strong> ${escapeHtml(title)}<span class="block text-xs text-muted-foreground">${escapeHtml(activityPlural(item.value, item.milestone.one, item.milestone.unit))}</span></span>
      </li>`;
    }).join("");
    const hint = next ? `<div class="mt-3 min-w-0">
        <p class="flex min-w-0 items-center gap-2 text-sm text-foreground">${activityMedalHtml(next.milestone, next.tier, 20, activityMilestoneTitle(next.milestone, next.tier))}<span class="min-w-0">${formatCount(next.target - next.value)} more ${escapeHtml(next.target - next.value === 1 ? next.milestone.one : next.milestone.unit)} to reach <strong>${escapeHtml(activityMilestoneTitle(next.milestone, next.tier))}</strong></span></p>
        ${activityProgressHtml(next.value, next.target, `Progress to ${activityMilestoneTitle(next.milestone, next.tier)}`)}
        <p class="mt-1 text-xs text-muted-foreground tabular-nums">${formatCount(next.value)} of ${formatCount(next.target)} ${escapeHtml(next.milestone.unit)}</p>
      </div>` : "";
    return `<section data-activity-you class="rounded-md border border-border p-4">
      <div class="flex min-w-0 flex-wrap items-center gap-x-4 gap-y-2">
        <span class="flex min-w-0 items-center gap-2">${activityAvatarHtml(person)}<span class="text-sm font-semibold text-foreground">Your activity</span></span>
        <span class="flex flex-wrap items-center gap-x-4 gap-y-1 text-sm text-muted-foreground">${stats.join("")}</span>
      </div>
      ${celebrations ? `<ul class="mt-3 grid gap-2">${celebrations}</ul>` : ""}
      ${hint}
    </section>`;
  }

  function activityCelebrate(root) {
    const reduce = window.matchMedia?.("(prefers-reduced-motion: reduce)").matches;
    root?.querySelectorAll("[data-activity-celebrate]").forEach((holder) => {
      const key = `fm-activity-celebrated:${holder.dataset.activityCelebrate || ""}`;
      let seen = false;
      try {
        seen = window.localStorage.getItem(key) === "1";
        window.localStorage.setItem(key, "1");
      } catch (_) {}
      if (seen || reduce) return;
      const burst = document.createElement("span");
      burst.className = "fm-activity-burst";
      burst.setAttribute("aria-hidden", "true");
      for (let index = 0; index < 14; index += 1) {
        const piece = document.createElement("i");
        piece.style.setProperty("--angle", `${index * (360 / 14)}deg`);
        piece.style.setProperty("--hue", String((index * 47) % 360));
        piece.style.setProperty("--delay", `${index * 15}ms`);
        burst.append(piece);
      }
      holder.append(burst);
      window.setTimeout(() => burst.remove(), 1400);
    });
  }

  function activityMoveHtml(row, scopeAll) {
    const now = Number(row.rank) || 0;
    const before = Number(row.previousRank) || 0;
    const compared = scopeAll ? "a week ago" : "the previous period";
    if (!before && row.newcomer) return "";
    if (!before) return `<span class="fm-label fm-label-accent" title="Not ranked in ${compared}">New</span>`;
    if (before === now) return `<span class="inline-flex items-center gap-0.5 text-xs text-muted-foreground" title="Same rank as ${compared}"><i data-lucide="minus" class="h-3.5 w-3.5" aria-hidden="true"></i><span class="sr-only">Same rank as ${compared}</span></span>`;
    const up = before > now;
    const places = Math.abs(before - now);
    const label = `${up ? "Up" : "Down"} ${activityPlural(places, "place")} since ${compared}`;
    return `<span class="inline-flex items-center gap-0.5 text-xs font-medium ${up ? "text-success" : "text-destructive"}" title="${escapeHtml(label)}"><span aria-hidden="true">${up ? "▲" : "▼"}</span>${formatCount(places)}<span class="sr-only">${escapeHtml(label)}</span></span>`;
  }

  function activityBreakdownLines(breakdown, weights) {
    return ACTIVITY_SCORE_PARTS.map((part) => {
      const count = Number(breakdown?.[part.key]) || 0;
      const weight = Number(weights?.[part.key]) || 0;
      return { ...part, count, weight, points: count * weight };
    });
  }

  function activityScoreHtml(row, weights, index) {
    const lines = activityBreakdownLines(row.breakdown, weights).filter((line) => line.count);
    const tipId = `activity-score-${index}`;
    const tip = lines.map((line) => `<span class="flex items-center gap-2"><span class="fm-activity-swatch" style="background:${line.color}" aria-hidden="true"></span>${formatCount(line.count)} ${escapeHtml(line.count === 1 ? line.one : line.label)} × ${formatCount(line.weight)} = ${formatCount(line.points)}</span>`).join("");
    const bar = lines.length ? `<span class="flex h-1.5 w-12 overflow-hidden rounded-full bg-secondary" aria-hidden="true">${lines.map((line) => `<span style="width:${(line.points / Math.max(1, row.score) * 100).toFixed(1)}%;background:${line.color}"></span>`).join("")}</span>` : "";
    return `<span class="fm-activity-tip-wrap justify-end"><button type="button" class="fm-activity-score" aria-describedby="${tipId}">${bar}${formatCount(row.score)}<span class="sr-only"> points</span></button><span role="tooltip" id="${tipId}" class="fm-tooltip fm-activity-tip"><span class="grid gap-1">${tip || "No scored activity"}</span></span></span>`;
  }

  function activityPartsHtml(row, limit = 4) {
    const parts = ACTIVITY_SCORE_PARTS
      .map((part) => ({ part, count: Number(row?.breakdown?.[part.key]) || 0 }))
      .filter((item) => item.count)
      .slice(0, limit);
    if (!parts.length) return "";
    return `<span class="fm-activity-parts" data-activity-parts>${parts.map(({ part, count }) => `<span><span class="fm-activity-swatch" style="background:${part.color}" aria-hidden="true"></span>${formatCount(count)} ${escapeHtml(part.short[count === 1 ? 0 : 1])}</span>`).join("")}</span>`;
  }

  function activityPodiumHtml(rows) {
    return `<div class="fm-activity-podium" data-activity-podium>${rows.map((row) => `<div class="fm-activity-podium-card" data-podium="${row.rank}">
        <span class="fm-activity-rank" data-podium="${row.rank}"><span class="sr-only">Rank </span>${formatCount(row.rank)}</span>
        <span class="fm-activity-podium-avatar">${activityAvatarHtml(row)}</span>
        <span class="flex min-w-0 max-w-full justify-center">${activityNameHtml(row.name)}</span>
        <span class="fm-activity-podium-score">${formatCount(row.score)}<span class="ml-1 text-xs font-normal text-muted-foreground">pts</span></span>
        ${activityPartsHtml(row, 3)}
      </div>`).join("")}</div>`;
  }

  function activityLeaderboardRowsHtml(data, scopeAll) {
    const all = Array.isArray(data?.leaderboard) ? data.leaderboard : [];
    const weights = data?.scoring?.weights || {};
    const podium = all.length >= 3 ? all.slice(0, 3) : [];
    const rows = all.slice(podium.length);
    if (!all.length) {
      return `<div class="fm-blankslate"><i data-lucide="trophy" aria-hidden="true"></i><p class="fm-blankslate-title">No scored activity yet</p><p class="fm-blankslate-desc">Merged pull requests, commits, reviews, closed issues and comments ${scopeAll ? "in this repository" : escapeHtml(activityPeriod(activityView.period).phrase)} will rank contributors here.</p></div>`;
    }
    return `${podium.length ? activityPodiumHtml(podium) : ""}<ol class="divide-y divide-border" data-activity-leaderboard-rows>${rows.map((row, index) => {
      const streak = Number(row.streak) || 0;
      const medals = activityEarned(row.achievements).map((item) => activityMedalHtml(item.milestone, item.tier, 20, `${activityMilestoneTitle(item.milestone, item.tier)} — ${activityPlural(item.value, item.milestone.one, item.milestone.unit)}`)).join("");
      return `<li class="fm-activity-row px-4 py-2.5" data-activity-rank-row>
        <span class="fm-activity-rank"${row.rank <= 3 ? ` data-podium="${row.rank}"` : ""}><span class="sr-only">Rank </span>${formatCount(row.rank)}</span>
        <span class="fm-activity-move-wide items-center">${activityMoveHtml(row, scopeAll)}</span>
        <span class="flex min-w-0 items-center gap-2">${activityAvatarHtml(row)}<span class="min-w-0"><span class="flex min-w-0 items-center gap-2">${activityNameHtml(row.name)}</span><span class="flex flex-wrap items-center gap-2 text-xs text-muted-foreground"><span class="fm-activity-move-narrow">${activityMoveHtml(row, scopeAll)}</span>${row.newcomer ? '<span class="fm-label fm-label-accent shrink-0"><i data-lucide="sparkles" aria-hidden="true"></i>New here</span>' : ""}${streak >= 2 ? `<span class="inline-flex items-center gap-1"><i data-lucide="flame" class="h-3.5 w-3.5 ${streak >= 3 ? "text-attention" : ""}" aria-hidden="true"></i>${formatCount(streak)}-day streak</span>` : ""}</span>${activityPartsHtml(row)}</span></span>
        <span class="fm-activity-medals items-center gap-1">${medals}</span>
        ${activityScoreHtml(row, weights, index)}
      </li>`;
    }).join("")}</ol>`;
  }

  function activityScoringHtml(data) {
    const scoring = data?.scoring || {};
    const weights = scoring.weights || {};
    return `<details class="border-t border-border px-4 py-3 text-sm" data-activity-scoring>
      <summary class="cursor-pointer text-foreground">How the score works</summary>
      <ul class="mt-2 grid gap-1 text-muted-foreground">${ACTIVITY_SCORE_PARTS.map((part) => `<li class="flex items-center gap-2"><span class="fm-activity-swatch" style="background:${part.color}" aria-hidden="true"></span><strong class="text-foreground">${formatCount(weights[part.key] || 0)} ${Number(weights[part.key]) === 1 ? "point" : "points"}</strong> per ${escapeHtml(part.one)}</li>`).join("")}</ul>
      <p class="mt-2 text-xs leading-5 text-muted-foreground">At most ${formatCount(scoring.commitsPerDay || 0)} commits a day and ${formatCount(scoring.commentsPerThread || 0)} comments per issue or pull request count, a review counts once per pull request and never on your own, slash commands are not comments, and at most ${formatCount(scoring.openedPerDay || 0)} pull requests and ${formatCount(scoring.openedPerDay || 0)} issues opened a day count. Equal scores are ordered by ${escapeHtml((scoring.tieBreak || []).map((key) => ACTIVITY_TIE_LABELS[key] || key).join(", then "))}, so no two people share a rank. Bots, AI agents and mirror nodes aren't ranked, and their commits are left out of the counts. Arrows compare with ${activityView.scope === "all" ? "the ranking a week ago" : "the previous period of the same length"}.</p>
    </details>`;
  }

  function activityLeaderboardHtml(data, scopeAll) {
    const total = Number(data?.leaderboardTotal) || 0;
    const shown = Array.isArray(data?.leaderboard) ? data.leaderboard.length : 0;
    const title = `${scopeAll ? "All-time ranking" : `Ranking ${activityPeriod(activityView.period).phrase}`}${total > shown ? ` · top ${formatCount(shown)} of ${formatCount(total)}` : ""}`;
    return activitySection("activity-leaderboard", escapeHtml(title), activityLeaderboardRowsHtml(data, scopeAll) + activityScoringHtml(data), '<i data-lucide="trophy" class="h-4 w-4 text-muted-foreground" aria-hidden="true"></i>', " data-activity-leaderboard");
  }

  function activityPeriodControls() {
    return activitySegmented("data-activity-period", ACTIVITY_PERIODS, activityView.period, "Period");
  }

  function activityPulseHtml(repo, data, person) {
    const period = activityPeriod(activityView.period);
    const pulls = data.pulls || {};
    const issues = data.issues || {};
    const top = Array.isArray(data.leaderboard) ? data.leaderboard.slice(0, 5) : [];
    const podium = top.length ? activitySection("activity-podium", `Top contributors ${escapeHtml(period.phrase)}`, `<ol class="divide-y divide-border">${top.map((row) => `<li class="flex min-w-0 items-center gap-3 px-4 py-2"><span class="fm-activity-rank"${row.rank <= 3 ? ` data-podium="${row.rank}"` : ""}><span class="sr-only">Rank </span>${formatCount(row.rank)}</span>${activityAvatarHtml(row)}${activityNameHtml(row.name)}<span class="ml-auto shrink-0 text-sm font-semibold tabular-nums text-foreground">${formatCount(row.score)}<span class="font-normal text-muted-foreground"> pts</span></span></li>`).join("")}</ol><a href="${escapeHtml(activityHref(repo, "leaderboard", activityView.period, "period"))}" data-activity-view="leaderboard" class="flex items-center justify-center gap-1.5 border-t border-border px-4 py-2 text-[13px] text-primary hover:bg-secondary/40">Open the leaderboard</a>`, '<i data-lucide="trophy" class="h-4 w-4 text-muted-foreground" aria-hidden="true"></i>') : "";
    return `
      ${activityOverviewHtml(data)}
      ${activitySummaryHtml(data)}
      <div data-activity-you-slot>${person}</div>
      <div class="grid min-w-0 gap-5 lg:grid-cols-2">${podium}${activityNewcomersHtml(data)}</div>
      ${activityListHtml(repo, "activity-merged", "pulls", pulls.merged, `${activityPlural(pulls.merged?.count || 0, "pull request")} merged by ${activityPlural(pulls.merged?.authors || 0, "person", "people")}`, '<i data-lucide="git-merge" class="h-4 w-4 fm-activity-done-text" aria-hidden="true"></i>', `No pull requests were merged ${period.phrase}.`)}
      ${activityListHtml(repo, "activity-proposed", "pulls", pulls.opened, `${activityPlural(pulls.opened?.count || 0, "pull request")} opened by ${activityPlural(pulls.opened?.authors || 0, "person", "people")}`, '<i data-lucide="git-pull-request" class="h-4 w-4 fm-activity-open-text" aria-hidden="true"></i>', `No pull requests were opened ${period.phrase}.`)}
      ${activityListHtml(repo, "activity-closed", "issues", issues.closed, `${activityPlural(issues.closed?.count || 0, "issue")} closed`, '<i data-lucide="circle-check" class="h-4 w-4 fm-activity-done-text" aria-hidden="true"></i>', `No issues were closed ${period.phrase}.`)}
      ${activityListHtml(repo, "activity-opened", "issues", issues.opened, `${activityPlural(issues.opened?.count || 0, "issue")} opened by ${activityPlural(issues.opened?.authors || 0, "person", "people")}`, '<i data-lucide="circle-dot" class="h-4 w-4 fm-activity-open-text" aria-hidden="true"></i>', `No issues were opened ${period.phrase}.`)}`;
  }

  function activityUnavailableHtml(message) {
    return `<div class="fm-blankslate rounded-md border border-border"><i data-lucide="cloud-off" aria-hidden="true"></i><p class="fm-blankslate-title">Activity is unavailable</p><p class="fm-blankslate-desc">${escapeHtml(message)}</p><button type="button" data-repo-insights-refresh class="fm-btn fm-btn-sm mt-2">Try again</button></div>`;
  }

  function activityAccount() {
    const session = state.session;
    return session?.sessionToken && session?.nodeName ? String(session.nodeName) : "";
  }

  async function activityFetchPulse(repo, period, person = "") {
    const key = `${repoKey(repo)}|${period}|${person.toLowerCase()}`;
    const cache = person ? activityView.person : activityView.pulse;
    const cached = cache.get(key);
    if (cached && Date.now() - cached.at < 60000) return cached.data;
    const params = { period, ref: "" };
    if (person) params.person = person;
    const data = await fetchRepoJson(repoLiveUrl(repo, "pulse", params));
    if (cache.size > 24) cache.clear();
    cache.set(key, { at: Date.now(), data });
    return data;
  }

  async function activityLoadCatalog() {
    if (activityView.catalog) return activityView.catalog;
    try {
      const body = await fetchJson("/api/badges");
      activityView.catalog = Array.isArray(body?.badges) ? body.badges : [];
    } catch (_) {
      activityView.catalog = [];
    }
    return activityView.catalog;
  }

  function activityFinish(root) {
    if (!root) return;
    activityHydrateMedals(root);
    dropBrokenContributorAvatars(root);
    window.lucide?.createIcons();
  }

  async function activityRenderPerson(repo, seq, scopeAll) {
    const name = activityAccount();
    const slot = $("[data-activity-you-slot]");
    if (!name || !slot) return;
    try {
      const [data] = await Promise.all([
        activityFetchPulse(repo, scopeAll ? "all" : activityView.period, name),
        activityLoadCatalog(),
      ]);
      if (seq !== activityView.seq) return;
      slot.innerHTML = activityYouHtml(repo, data?.person, scopeAll);
      activityFinish(slot);
      activityCelebrate(slot);
    } catch (_) {
      if (seq === activityView.seq) slot.innerHTML = "";
    }
  }

  async function activityRenderPulse(repo, main, seq) {
    const period = activityPeriod(activityView.period);
    const signedIn = Boolean(activityAccount());
    main.innerHTML = activityHeaderHtml("Pulse", "Loading activity…", activityPeriodControls())
      + activitySkeleton([186, 236, ...(signedIn ? [96] : []), 160, 120, 120]);
    let data;
    try {
      [data] = await Promise.all([activityFetchPulse(repo, activityView.period), activityLoadCatalog()]);
    } catch (error) {
      if (seq !== activityView.seq) return;
      main.innerHTML = activityHeaderHtml("Pulse", "", activityPeriodControls()) + activityUnavailableHtml(error?.status === 404
        ? "This repository's host does not serve activity summaries yet."
        : "The repository host could not be reached. Try again in a moment.");
      return;
    }
    if (seq !== activityView.seq) return;
    const subtitle = `${escapeHtml(activityRange(data))} · ${escapeHtml(period.label)}`;
    const you = signedIn ? `<span class="fm-activity-skeleton" style="height:96px"></span>` : "";
    main.innerHTML = activityHeaderHtml("Pulse", subtitle, activityPeriodControls()) + activityPulseHtml(repo, data, you);
    activityFinish(main);
    void activityRenderPerson(repo, seq, false);
  }

  async function activityRenderLeaderboard(repo, main, seq) {
    const scopeAll = activityView.scope === "all";
    const signedIn = Boolean(activityAccount());
    const controls = `${activitySegmented("data-activity-scope", [{ id: "period", label: "This period" }, { id: "all", label: "All time" }], activityView.scope, "Leaderboard scope")}${scopeAll ? "" : activityPeriodControls()}`;
    main.innerHTML = activityHeaderHtml("Leaderboard", "Loading the ranking…", controls) + activitySkeleton([...(signedIn ? [96] : []), 420]);
    let data;
    try {
      [data] = await Promise.all([activityFetchPulse(repo, scopeAll ? "all" : activityView.period), activityLoadCatalog()]);
    } catch (error) {
      if (seq !== activityView.seq) return;
      main.innerHTML = activityHeaderHtml("Leaderboard", "", controls) + activityUnavailableHtml(error?.status === 404
        ? "This repository's host does not serve activity summaries yet."
        : "The repository host could not be reached. Try again in a moment.");
      return;
    }
    if (seq !== activityView.seq) return;
    const subtitle = scopeAll
      ? `Contribution score across the whole history · ${escapeHtml(activityRange(data))}`
      : `Contribution score ${escapeHtml(activityPeriod(activityView.period).phrase)} · ${escapeHtml(activityRange(data))}`;
    const you = signedIn ? `<span class="fm-activity-skeleton" style="height:96px"></span>` : "";
    main.innerHTML = activityHeaderHtml("Leaderboard", subtitle, controls) + `<div data-activity-you-slot>${you}</div>` + activityLeaderboardHtml(data, scopeAll);
    activityFinish(main);
    void activityRenderPerson(repo, seq, scopeAll);
  }

  async function activityInsightsData(repo) {
    if (activityView.insights && Date.now() - activityView.insights.at < 60000) return activityView.insights;
    const [historyResult, statsResult, trafficResult] = await Promise.allSettled([
      fetchRepoJson(repoLiveUrl(repo, "history")),
      fetchRepoJson(repoLiveUrl(repo, "stats")),
      fetchJson(`${repoApiBase(repo)}/traffic`),
    ]);
    const unavailable = historyResult.status === "rejected" && statsResult.status === "rejected";
    activityView.insights = {
      at: Date.now(),
      unavailable,
      commits: historyResult.status === "fulfilled" && Array.isArray(historyResult.value?.commits) ? historyResult.value.commits : [],
      stats: statsResult.status === "fulfilled" ? statsResult.value : null,
      traffic: trafficResult.status === "fulfilled" && trafficResult.value?.ok ? trafficResult.value : null,
    };
    return activityView.insights;
  }

  async function activityRenderContributors(repo, main, seq) {
    main.innerHTML = activityHeaderHtml("Contributors", "Loading contributors…", "") + activitySkeleton([140, 120, 360]);
    const data = await activityInsightsData(repo);
    if (seq !== activityView.seq) return;
    const total = Number(data.stats?.contributorCount) || 0;
    main.innerHTML = activityHeaderHtml("Contributors", total ? `${escapeHtml(activityPlural(total, "contributor"))} to ${escapeHtml(repoSelectedBranch(repo) || "this branch")}` : "Everyone who has committed to this branch", "")
      + `<div data-repo-insights-body class="min-w-0">${renderRepoInsights(repo, data.commits, data.stats, data.unavailable, data.traffic)}</div>`;
    hydrateRepoActivityGraphs(main);
    dropBrokenContributorAvatars(main);
    window.lucide?.createIcons();
  }

  function activityWeeksHtml(weeks, start) {
    const max = Math.max(1, ...weeks);
    const total = weeks.reduce((sum, value) => sum + value, 0);
    const label = (index) => {
      const date = new Date(start.getTime() + index * 7 * 86400000);
      return date.toLocaleDateString(undefined, { month: "short", day: "numeric" });
    };
    return `<div class="px-4 pb-4 pt-4">
      <div class="fm-activity-weeks" role="img" aria-label="${escapeHtml(`${activityPlural(total, "commit")} over the last 52 weeks; the busiest week had ${activityPlural(max, "commit")}`)}">${weeks.map((value, index) => `<span title="${escapeHtml(`Week of ${label(index)}: ${activityPlural(value, "commit")}`)}" style="height:${value ? Math.max(3, Math.round(value / max * 100)) : 1}%;${value ? "" : "opacity:.35"}"></span>`).join("")}</div>
      <div class="mt-2 flex justify-between text-xs text-muted-foreground"><span>${escapeHtml(label(0))}</span><span>${escapeHtml(label(weeks.length - 1))}</span></div>
    </div>`;
  }

  function activityDiffHtml(series) {
    const buckets = Array.isArray(series?.buckets) ? series.buckets : [];
    if (!buckets.length) return "";
    const step = Number(series.stepSeconds) || 86400;
    const start = new Date(series.start || Date.now());
    const max = Math.max(1, ...buckets.map((bucket) => Math.max(Number(bucket.additions) || 0, Number(bucket.deletions) || 0)));
    const label = (index) => {
      const date = new Date(start.getTime() + index * step * 1000);
      return step < 86400
        ? date.toLocaleString(undefined, { month: "short", day: "numeric", hour: "numeric" })
        : date.toLocaleDateString(undefined, { month: "short", day: "numeric" });
    };
    return `<div class="px-4 pb-4 pt-4">
      <div class="fm-activity-diff" role="list" aria-label="Additions and deletions over the period">${buckets.map((bucket, index) => {
        const additions = Number(bucket.additions) || 0;
        const deletions = Number(bucket.deletions) || 0;
        const text = `${label(index)}: ${activityPlural(additions, "addition")}, ${activityPlural(deletions, "deletion")}, ${activityPlural(Number(bucket.commits) || 0, "commit")}`;
        return `<span role="listitem" aria-label="${escapeHtml(text)}" title="${escapeHtml(text)}"><span><i class="bg-success" style="height:${additions ? Math.max(2, Math.round(additions / max * 100)) : 0}%"></i></span><span><i class="bg-destructive" style="height:${deletions ? Math.max(2, Math.round(deletions / max * 100)) : 0}%"></i></span></span>`;
      }).join("")}</div>
      <div class="mt-2 flex justify-between text-xs text-muted-foreground"><span>${escapeHtml(label(0))}</span><span>${escapeHtml(label(buckets.length - 1))}</span></div>
    </div>`;
  }

  async function activityRenderFrequency(repo, main, seq) {
    const period = activityPeriod(activityView.period);
    main.innerHTML = activityHeaderHtml("Code frequency", "Loading…", activityPeriodControls()) + activitySkeleton([236, 196]);
    const [insights, pulse] = await Promise.all([
      activityInsightsData(repo),
      activityFetchPulse(repo, activityView.period).catch(() => null),
    ]);
    if (seq !== activityView.seq) return;
    const weeks = Array(52).fill(0);
    (Array.isArray(insights.stats?.contributors) ? insights.stats.contributors : []).forEach((item) => {
      (Array.isArray(item.activityWeeks) ? item.activityWeeks.slice(-52) : []).forEach((value, index, list) => {
        weeks[52 - list.length + index] += Math.max(0, Number(value) || 0);
      });
    });
    const weekStart = insights.stats?.activityStart ? new Date(insights.stats.activityStart) : new Date(Date.now() - 51 * 7 * 86400000);
    const weekly = insights.stats
      ? activitySection("activity-weeks", "Commits per week · last 52 weeks", activityWeeksHtml(weeks, weekStart), '<i data-lucide="chart-column" class="h-4 w-4 text-muted-foreground" aria-hidden="true"></i>')
      : activitySection("activity-weeks", "Commits per week", '<p class="px-4 py-4 text-sm text-muted-foreground">Weekly history is unavailable from this repository host.</p>');
    const summary = pulse?.summary || null;
    const diff = pulse
      ? activitySection("activity-diff", `Additions and deletions ${escapeHtml(period.phrase)}`, `<p class="px-4 pt-3 text-sm text-foreground"><strong class="text-success">+${formatCount(summary?.additions || 0)}</strong> <strong class="text-destructive">−${formatCount(summary?.deletions || 0)}</strong> <span class="text-muted-foreground">across ${escapeHtml(activityPlural(summary?.main || 0, "commit"))} on <span class="font-mono">${escapeHtml(pulse.branch || "the default branch")}</span>, merges excluded</span></p>${activityDiffHtml(pulse.series)}`, '<i data-lucide="diff" class="h-4 w-4 text-muted-foreground" aria-hidden="true"></i>')
      : activitySection("activity-diff", "Additions and deletions", '<p class="px-4 py-4 text-sm text-muted-foreground">Line counts are unavailable from this repository host.</p>');
    main.innerHTML = activityHeaderHtml("Code frequency", `${escapeHtml(pulse ? activityRange(pulse) : "")}${pulse ? " · " : ""}${escapeHtml(period.label)}`, activityPeriodControls()) + weekly + diff;
    window.lucide?.createIcons();
  }

  async function activityRenderCommits(repo, main, seq) {
    main.innerHTML = activityHeaderHtml("Recent commits", "Loading commits…", "") + activitySkeleton([420]);
    const data = await activityInsightsData(repo);
    if (seq !== activityView.seq) return;
    const branch = repoSelectedBranch(repo) || "this branch";
    const commits = data.commits.slice(0, 60);
    const groups = [];
    commits.forEach((commit) => {
      const date = new Date(commit.date || 0);
      const day = Number.isNaN(date.getTime()) ? "Unknown date" : date.toLocaleDateString(undefined, { month: "short", day: "numeric", year: "numeric" });
      const last = groups[groups.length - 1];
      if (last && last.day === day) last.items.push(commit);
      else groups.push({ day, items: [commit] });
    });
    const body = groups.length ? groups.map((group) => activitySection("", `<i data-lucide="git-commit-horizontal" class="h-4 w-4 text-muted-foreground" aria-hidden="true"></i>Commits on ${escapeHtml(group.day)}`, `<ul class="divide-y divide-border">${group.items.map((commit) => {
      const hash = String(commit.hash || "");
      return `<li class="flex min-w-0 items-center gap-3 px-4 py-2.5">
        ${activityAvatarHtml({ name: shownAccountName(commit.author) })}
        <span class="min-w-0 flex-1">
          <button type="button" data-dashboard-commit-hash="${escapeHtml(hash)}" class="block w-full truncate text-left text-[14px] font-medium text-foreground hover:text-accent hover:underline">${escapeHtml(commit.subject || "Commit")}</button>
          <span class="flex min-w-0 items-center gap-1 text-xs text-muted-foreground">${activityNameHtml(commit.author, "text-muted-foreground")}<span aria-hidden="true">·</span><time datetime="${escapeHtml(commit.date || "")}">${escapeHtml(formatTimeAgo(commit.date))}</time></span>
        </span>
        <button type="button" data-dashboard-commit-hash="${escapeHtml(hash)}" class="shrink-0 font-mono text-xs text-muted-foreground hover:text-foreground hover:underline" aria-label="Open commit ${escapeHtml(hash.slice(0, 7))}">${escapeHtml(hash.slice(0, 7))}</button>
      </li>`;
    }).join("")}</ul>`)).join("") : activityUnavailableHtml(data.unavailable ? "The repository host could not be reached. Try again in a moment." : "No commits are available yet.");
    main.innerHTML = activityHeaderHtml("Recent commits", `The latest ${escapeHtml(activityPlural(commits.length, "commit"))} on <span class="font-mono">${escapeHtml(branch)}</span> · <a href="${escapeHtml(`${repoLinkUrl(repo)}/commits`)}" data-dashboard-repo-tab="commits" class="text-primary hover:underline">Full history</a>`, "") + body;
    activityFinish(main);
  }

  function activityRender(repo) {
    const main = $("[data-activity-main]");
    if (!main || !repo) return;
    activityView.seq += 1;
    const seq = activityView.seq;
    $$("[data-repo-activity] [data-activity-view]").forEach((link) => {
      const view = link.dataset.activityView;
      link.href = activityHref(repo, view, activityView.period, activityView.scope);
      if (link.closest("nav")) {
        if (view === activityView.view) link.setAttribute("aria-current", "page");
        else link.removeAttribute("aria-current");
      }
    });
    activityRememberLocation(repo);
    main.setAttribute("aria-busy", "true");
    const render = {
      pulse: activityRenderPulse,
      leaderboard: activityRenderLeaderboard,
      contributors: activityRenderContributors,
      frequency: activityRenderFrequency,
      commits: activityRenderCommits,
    }[activityView.view] || activityRenderPulse;
    Promise.resolve(render(repo, main, seq)).catch(() => {
      if (seq === activityView.seq) main.innerHTML = activityUnavailableHtml("Something went wrong while drawing this view.");
    }).finally(() => {
      if (seq !== activityView.seq) return;
      main.setAttribute("aria-busy", "false");
      window.lucide?.createIcons();
    });
  }

  function activityOpenRecord(repo, kind, number) {
    state.loadedRepoTabs = state.loadedRepoTabs || {};
    state.loadedRepoTabs[kind] = true;
    setRepoTab(kind);
    state.repoRecordSection = "conversation";
    navigateHistory(repoRecordUrl(repo, kind, number, "conversation"));
    loadRepoRecordDetail(repo, kind, number);
  }

  function activityBind(container) {
    if (activityView.bound === container) return;
    activityView.bound = container;
    container.addEventListener("click", (event) => {
      const repo = state.selectedRepo;
      if (!repo) return;
      const plain = event.button === 0 && !event.metaKey && !event.ctrlKey && !event.shiftKey && !event.altKey;
      const viewLink = event.target.closest("[data-activity-view]");
      if (viewLink && plain) {
        event.preventDefault();
        activityView.view = viewLink.dataset.activityView || "pulse";
        if (viewLink.closest("[data-activity-podium], #activity-podium")) activityView.scope = "period";
        activityRender(repo);
        $("[data-repo-activity]")?.scrollIntoView?.({ block: "nearest" });
        return;
      }
      const periodButton = event.target.closest("[data-activity-period]");
      if (periodButton) {
        activityView.period = periodButton.dataset.activityPeriod || ACTIVITY_DEFAULT_PERIOD;
        activityView.expanded.clear();
        activityRender(repo);
        return;
      }
      const scopeButton = event.target.closest("[data-activity-scope]");
      if (scopeButton) {
        activityView.scope = scopeButton.dataset.activityScope === "all" ? "all" : "period";
        activityRender(repo);
        return;
      }
      const more = event.target.closest("[data-activity-more]");
      if (more) {
        const id = more.dataset.activityMore || "";
        const open = !activityView.expanded.has(id);
        if (open) activityView.expanded.add(id);
        else activityView.expanded.delete(id);
        const section = more.closest("[data-activity-list]");
        section?.querySelectorAll("[data-activity-item]").forEach((item, index) => {
          item.classList.toggle("hidden", !open && index >= ACTIVITY_LIST_PREVIEW);
        });
        more.setAttribute("aria-expanded", open ? "true" : "false");
        const label = more.querySelector("span");
        if (label) label.textContent = open ? "Show fewer" : `Show all ${formatCount(section?.querySelectorAll("[data-activity-item]").length || 0)}`;
        if (open) section?.querySelectorAll("[data-activity-item]")[ACTIVITY_LIST_PREVIEW]?.querySelector("a")?.focus?.();
        return;
      }
      const jump = event.target.closest("[data-activity-jump]");
      if (jump && plain) {
        event.preventDefault();
        const target = document.getElementById(jump.dataset.activityJump || "");
        target?.scrollIntoView?.({ behavior: "smooth", block: "start" });
        target?.querySelector("a, button")?.focus?.({ preventScroll: true });
        return;
      }
      const record = event.target.closest("[data-activity-record]");
      if (record && plain) {
        event.preventDefault();
        activityOpenRecord(repo, record.dataset.activityRecord, record.dataset.activityNumber);
      }
    });
  }

  async function loadRepoActivity(repo) {
    const container = $("[data-repo-insights]");
    if (!container || !repo) return;
    activityEnsureStyle();
    const key = repoKey(repo);
    const fromUrl = activityReadLocation();
    if (/\/insights\/?$/.test(location.pathname) || activityView.repoKey !== key) {
      activityView.view = fromUrl.view;
      activityView.period = fromUrl.period;
      activityView.scope = fromUrl.scope;
    }
    if (activityView.repoKey === key && container.querySelector("[data-repo-activity]")) {
      activityView.pulse.clear();
      activityView.person.clear();
      activityView.insights = null;
    } else {
      activityView.repoKey = key;
      activityView.pulse.clear();
      activityView.person.clear();
      activityView.insights = null;
      activityView.expanded.clear();
      container.innerHTML = activityShellHtml(repo);
    }
    activityBind(container);
    activityRender(repo);
  }


  function setRepoForkMenuOpen(opening) {
    $("[data-repo-fork-menu]")?.classList.toggle("hidden", !opening);
    $("[data-repo-action-fork]")?.setAttribute("aria-expanded", opening ? "true" : "false");
    if (opening && state.selectedRepo) {
      loadRepoForks(state.selectedRepo);
      void loadRepoRemoteForks(state.selectedRepo, { force: true });
    }
  }

  function openRepoForkMenuFromMirror() {
    const detail = $("[data-repo-detail]");
    const about = detail?.querySelector("[data-repo-about]");
    if (!about) return;
    if (about.classList.contains("hidden")) activateRepoTab("code");
    if (about.classList.contains("hidden")) setRepoExplorerFocusMode(false);
    setRepoForkMenuOpen(true);
    $("[data-repo-fork-wrap]")?.scrollIntoView({ block: "center", behavior: "smooth" });
    ($("[data-repo-fork-create]") || $("[data-repo-action-fork]"))?.focus({ preventScroll: true });
  }

  function applyRepoReadOnlyMirror(repo) {
    if (!repo) return;
    const readOnly = repoReadOnlyMirror(repo);
    const slot = $("[data-repo-provenance]");
    if (slot) {
      const provenance = repoProvenanceHtml(repo);
      slot.innerHTML = provenance;
      slot.classList.toggle("hidden", !provenance);
    }
    $$('[data-repo-file-action="edit"]').forEach((button) => {
      button.disabled = readOnly;
      button.title = readOnly ? REPO_READ_ONLY_MIRROR_HINT : "Edit file";
    });
    $$("[data-repo-pull-new], [data-repo-compare-pull]").forEach((button) => {
      button.classList.toggle("hidden", readOnly);
    });
    if (state.activeRepoTab === "branches") renderRepoBranchesList(repo);
    if (state.repoCompare) renderRepoCompareMergePanel(repo);
    window.lucide?.createIcons();
  }

  function renderRepoDetail(repo) {
    const detail = $("[data-repo-detail]");
    if (!detail || !repo) return;
    state.selectedRepo = repo;
    if (state.session?.nodeName && !sessionAdministersRepo(repo)
        && !repoViewerPermissions.has(repoViewerPermissionKey(repo))) {
      loadRepoViewerPermission(repo).then((answer) => {
        if (answer?.viewerCanAdmin && state.selectedRepo === repo) renderRepoDetail(repo);
      });
    }
    rememberAgentVisitedRepository(repo);
    recordRepositoryView(repo);
    state.repoCollectionPages = { issues: 1, pulls: 1 };
    const routeQuery = new URLSearchParams(location.search);
    const routeRef = String(routeQuery.get("ref") || "").trim();
    if (routeRef) setRepoSelectedBranch(repo, routeRef);
    state.repoHistoryPath = String(routeQuery.get("path") || "").trim();
    // The Branches page paints a fresh, empty filter box, so a query left
    // over from an earlier visit to this repo must not keep narrowing the
    // list behind it.
    setRepoBranchesQuery(repo, "");
    state.repoMirrors = [];
    state.repoSourceOfTruth = null;
    state.repoLatestCommit = null;
    state.repoServedBy = null;
    state.agentsView = { agents: [], selectedAgentId: null, canViewSessions: false, canDeleteSessions: false };
    // Issue and pull controls persist across refreshes and tab re-entry, but
    // must not carry into a different repository. Reset the whole view before
    // renderRepoCollectionPanel paints that repository's toolbar.
    state.issuesView = {
      filter: "open",
      items: [],
      query: "",
      filters: {
        author: "", label: String(routeQuery.get("label") || "").trim(), milestone: "", assignee: "",
        mentions: "", involves: "", type: "", sort: "recently_updated",
      },
    };
    state.pullsView = { filter: "open", query: "" };
    state.discussionsView = null;
    // Pull requests and discussions load lazily the first time their tab is
    // opened rather than on every page load. Eagerly fetching every record's
    // blob up front is what flooded the host with requests and tripped the rate
    // limit after a few refreshes; this map remembers which tabs have loaded.
    state.loadedRepoTabs = {};
    state.repoAboutLoaded = false;
    // Show the clean, shareable /owner/name URL in the address bar instead of
    // the /dashboard/owner/name... path that 404.html bounces refreshed repo
    // links (including /owner/name/issues etc.) to. Carry over whatever tab or
    // tree/blob suffix the incoming URL already pointed at instead of
    // collapsing it to the bare repo root - otherwise a refresh on the Issues
    // tab would lose its place and land back on Code. Only trust that suffix
    // when the URL is actually addressing THIS repo already (a fresh open from
    // the repo list/sidebar while some other repo's tab URL is showing should
    // still land on Code, not inherit the other repo's tab).
    const routeParts = repoRouteParts();
    const routeRepoKey = routeParts.length >= 2
      ? `${safeDecodeURIComponent(routeParts[0])}/${safeDecodeURIComponent(routeParts[1])}`
      : "";
    const routeMatchesRepo = routeParts.length >= 2
      && repoMatchesKey(repo, routeRepoKey);
    const routeKind = routeMatchesRepo ? routeParts[2] : undefined;
    const routePath = routeMatchesRepo && routeParts.length > 3 ? routeParts.slice(3).map(decodeURIComponent).join("/") : "";
    // /owner/repo/pulls/<N> is a record deep link (the desktop client's
    // "View on website" button, or a refreshed/shared PR detail URL): keep
    // the number in the address bar and open that record's detail page below.
    // Issues deep-link the same way so a refresh on an open issue stays on it.
    // A pull may carry its open section too (/pulls/<N>/files), so a link
    // copied from the Files changed review reopens on that tab.
    const recordNumber = routeMatchesRepo && routeParts.length > 3
      ? safeDecodeURIComponent(routeParts[3])
      : "";
    const recordRoute = ["pulls", "discussions", "issues"].includes(routeKind) && /^\d+$/.test(recordNumber)
      ? {
        kind: routeKind,
        number: recordNumber,
        section: routeKind === "pulls"
          ? normalizeRepoRecordSection(routeParts[4])
          : "conversation",
      }
      : null;
    const commitHash = routeKind === "commits" && /^[0-9a-f]{7,64}$/i.test(recordNumber)
      ? recordNumber
      : "";
    state.repoRecordSection = recordRoute ? recordRoute.section : "conversation";
    const actionsRunId = routeKind === "actions" && routeParts[3] === "runs" && /^[1-9]\d{0,11}$/.test(routeParts[4] || "")
      ? Number(routeParts[4])
      : 0;
    state.repoActionsInitialRun = actionsRunId ? { key: repoKey(repo), runId: actionsRunId } : null;
    // /owner/repo/compare/<base>...<head> opens the branch diff viewer on the
    // Pull requests tab (the Agents tab links an agent's branch there).
    const compareRoute = routeMatchesRepo ? parseRepoCompareRoute(routeParts) : null;
    const detailPath = recordRoute
      ? repoRecordUrl(repo, recordRoute.kind, recordRoute.number, recordRoute.section)
      : commitHash
        ? repoCommitUrl(repo, commitHash)
      : compareRoute
        ? repoCompareUrl(repo, compareRoute.base, compareRoute.head)
        : actionsRunId && repoTabRoutesFor(repo).includes("actions")
          ? `${repoLinkUrl(repo)}/actions/runs/${actionsRunId}`
          : routeKind === "pulls" && routeParts[3] === "send"
            ? pullBatchUrl(repo)
          : routeKind === "security"
            ? `${repoLinkUrl(repo)}/security${routeParts.slice(3).map((part) => `/${part}`).join("")}`
            : repoTabRoutesFor(repo).includes(routeKind)
              ? `${repoLinkUrl(repo)}/${routeKind}`
              : repoLinkUrl(repo, routeKind === "blob" ? "blob" : "tree", routePath);
    navigateHistory(detailPath);
    const branch = repoSelectedBranch(repo);
    const issuesCount = repoCount(repo, ["issueCount", "issues", "issuesCount", "openIssues"]);
    const pullsCount = repoCount(repo, ["pullCount", "pulls", "pullsCount", "openPulls", "pullRequests"]);
    const discussionsCount = repoCount(repo, ["discussions", "discussionCount"]);
    const commitsCount = repoCount(repo, ["commits", "commitCount", "commitHistory"]);
    const mirrorsCount = repoCount(repo, ["mirrors", "mirrorCount", "hosts"]);
    renderHeaderContext("explore");
    const canEditAbout = sessionAdministersRepo(repo);
    const readmePath = "README.md";
    const readmeHref = repoLinkUrl(repo, "blob", readmePath);
    const tabMeta = {
      code: { label: "Code", icon: "code-2", count: "" },
      insights: { label: "Insights", icon: "chart-no-axes-combined", count: "" },
      issues: { label: "Issues", icon: "circle-dot", count: issuesCount },
      projects: { label: "Projects", icon: "chart-gantt", count: "" },
      pulls: { label: "Pull requests", icon: "git-pull-request", count: pullsCount },
      actions: { label: "Actions", icon: "workflow", count: "" },
      security: { label: "Security", icon: "shield", count: "" },
      discussions: { label: "Discussions", icon: "message-square", count: discussionsCount },
      agents: { label: "Agents", icon: "bot", count: "" },
      settings: { label: "Settings", icon: "settings", count: "" },
    };
    const canSeeAgentsTab = sessionCanAssignAgent(repo);
    const canSeeSettingsTab = canEditAbout;
    const actionSeed = repoKey(repo);
    const websiteForkAvailable = !repo.isPrivate;
    // Watch is real: it is the repo's fediverse follower count (see
    // loadRepoFediverse), and the button opens the follow-from-Mastodon card.
    // The public repository actor is the canonical ForkMesh identity. Do not
    // derive it from a mirror hostname or replica owner name.
    const fediHandle = dashboardOnInstance()
      ? `@${String(repo.owner || "").toLowerCase()}.${String(repo.name || "").toLowerCase()}@${location.host}`
      : "@forkmesh.forkmesh@forkmesh.com";
    const mastodonUrl = fediverseFollowUrl(fediHandle);
    const mirrorButton = (compact) => canSeeSettingsTab
      ? `<a href="${escapeHtml(`${repoLinkUrl(repo)}/settings#mirror`)}" data-repo-forge-mirror-open title="Mirror this repository to GitHub or GitLab" class="inline-flex ${compact ? "h-8" : "h-9"} shrink-0 items-center justify-center gap-2 rounded-md border border-border bg-secondary px-3 text-xs font-semibold text-foreground hover:bg-background"><i data-lucide="arrow-left-right" class="h-3.5 w-3.5 text-muted-foreground"></i>Mirror</a>`
      : "";
    const settingsPanel = canSeeSettingsTab ? `
      <section data-dashboard-repo-tab-panel="settings" class="hidden">
        <div class="mt-4 overflow-hidden rounded-lg border border-border bg-background">
          <div class="border-b border-border bg-secondary/50 px-4 py-3">
            <span class="inline-flex items-center gap-2 text-xs font-medium text-foreground"><i data-lucide="settings" class="h-3.5 w-3.5 text-muted-foreground"></i>Repository settings</span>
          </div>
          <form data-repo-settings-form class="grid gap-4 p-4">
            <fieldset class="grid gap-2 rounded-md border border-border p-3 text-xs text-muted-foreground">
              <legend class="px-1 text-[11px] font-semibold uppercase tracking-wide">Features</legend>
              <label class="inline-flex items-start gap-2"><input data-repo-feature="issues" type="checkbox" class="mt-0.5 h-3.5 w-3.5" checked />Issues</label>
              <label class="inline-flex items-start gap-2"><input data-repo-feature="pulls" type="checkbox" class="mt-0.5 h-3.5 w-3.5" checked />Pull requests</label>
              <label class="inline-flex items-start gap-2"><input data-repo-feature="discussions" type="checkbox" class="mt-0.5 h-3.5 w-3.5" checked />Discussions</label>
              <label class="inline-flex items-start gap-2"><input data-repo-feature="actions" type="checkbox" class="mt-0.5 h-3.5 w-3.5" checked />Actions (run this repository's workflows)</label>
              <label class="inline-flex items-start gap-2"><input data-repo-feature="protectDefaultBranch" type="checkbox" class="mt-0.5 h-3.5 w-3.5" />Protect the default branch: it changes only by merging a pull request an organization owner or admin approved</label>
            </fieldset>
            <fieldset class="grid gap-2 rounded-md border border-border p-3 text-xs text-muted-foreground">
              <legend class="px-1 text-[11px] font-semibold uppercase tracking-wide">ActivityPub federation</legend>
              <label class="inline-flex items-start gap-2"><input data-repo-ap-federate type="checkbox" class="mt-0.5 h-3.5 w-3.5" checked />Federate this repository (fediverse actor and handle)</label>
              <label class="inline-flex items-start gap-2"><input data-repo-ap-broadcast type="checkbox" class="mt-0.5 h-3.5 w-3.5" checked />Include meaningful public updates in one automated digest at most every 24 hours</label>
              <label class="inline-flex items-start gap-2"><input data-repo-ap-comments type="checkbox" class="mt-0.5 h-3.5 w-3.5" checked />Accept fediverse replies as federated comments</label>
              ${repo.isPrivate ? "" : `
              <div class="mt-1 rounded-md border border-border bg-secondary/40 p-3">
                <div class="flex items-center justify-between gap-2">
                  <span class="font-semibold text-foreground">Daily digest preview</span>
                  <button type="button" data-repo-digest-preview-refresh class="rounded-md border border-border px-2 py-1 text-[10px] font-semibold text-foreground hover:bg-secondary">Refresh</button>
                </div>
                <p class="mt-1 leading-5">Only public titles, stable links, categories and UTC times enter the encrypted bounded queue. Empty digests are never posted.</p>
                <pre data-repo-digest-preview class="mt-2 max-h-64 overflow-auto whitespace-pre-wrap break-words rounded bg-background p-3 font-mono text-[11px] leading-5 text-foreground">Loading preview…</pre>
              </div>`}
            </fieldset>
            <p class="text-[11px] leading-5 text-muted-foreground">Saved to the relay now and written into the repository's committed <span class="font-mono">.forkmesh/info.json</span> the next time the owner's node syncs.</p>
            <div class="flex flex-wrap items-center justify-between gap-2">
              <span data-repo-settings-status class="text-[11px] text-muted-foreground"></span>
              <button type="submit" class="inline-flex h-8 items-center rounded-md bg-cta px-3 text-xs font-medium text-cta-foreground hover:bg-cta/90">Save settings</button>
            </div>
          </form>
        </div>
        <div data-repo-pull-rules-card class="mt-5 overflow-hidden rounded-lg border border-border bg-background">
          <div class="flex flex-wrap items-center justify-between gap-2 border-b border-border bg-secondary/50 px-4 py-3">
            <span class="inline-flex items-center gap-2 text-xs font-medium text-foreground"><i data-lucide="shield-check" class="h-3.5 w-3.5 text-muted-foreground"></i>Pull request rules</span>
            <span class="font-mono text-[10px] text-muted-foreground">.forkmesh/pull-rules.json</span>
          </div>
          <div class="grid gap-3 p-4">
            <p class="text-[11px] leading-5 text-muted-foreground">Every open pull request is checked against these rules and shows which ones it misses and how to fix them. A rule set to block keeps the merge locked until it passes; the repository host enforces it at merge time, so it cannot be waived. The rules are committed to the default branch, and a pull request is judged by the rules on its own base branch, so a proposal cannot loosen them. Rules an organization sets for its linked repositories apply as well.</p>
            <div data-repo-pull-rules-editor class="text-xs text-muted-foreground">Loading rules…</div>
          </div>
        </div>
        <div data-repo-webhooks-card class="mt-5 overflow-hidden rounded-lg border border-border bg-background">
          <div class="flex flex-wrap items-center justify-between gap-2 border-b border-border bg-secondary/50 px-4 py-3">
            <span class="inline-flex items-center gap-2 text-xs font-medium text-foreground"><i data-lucide="webhook" class="h-3.5 w-3.5 text-muted-foreground"></i>Webhooks</span>
            <span class="font-mono text-[10px] text-muted-foreground">forkmesh-webhook-v1 · signed</span>
          </div>
          <div class="grid gap-4 p-4">
            <p class="text-[11px] leading-5 text-muted-foreground">${escapeHtml(platformName())} POSTs a signed JSON event to each endpoint when this repository moves. Every delivery carries <span class="font-mono">ForkMesh-Webhook-Signature</span> (<span class="font-mono">v1=</span>HMAC-SHA256 over the raw body) so a receiver can prove it came from here. The scheme, the retry rules and a verification snippet are in the repository&rsquo;s <span class="font-mono">www/docs/integrations.md</span>.</p>
            <div data-repo-webhooks-list class="grid gap-2 text-xs text-muted-foreground">Loading webhooks…</div>
            <form data-repo-webhook-form class="grid gap-3 rounded-md border border-border p-3">
              <label class="grid gap-1 text-[11px] font-medium text-foreground">Payload URL
                <input data-repo-webhook-url type="url" inputmode="url" autocomplete="off" spellcheck="false" required placeholder="https://example.com/hooks/forkmesh" class="h-8 w-full rounded-md bg-secondary px-2 font-mono text-xs text-foreground outline-none placeholder:text-muted-foreground" />
              </label>
              <label class="grid gap-1 text-[11px] font-medium text-foreground">Description <span class="font-normal text-muted-foreground">(optional)</span>
                <input data-repo-webhook-description type="text" autocomplete="off" maxlength="120" placeholder="Codemagic CI" class="h-8 w-full rounded-md bg-secondary px-2 text-xs text-foreground outline-none placeholder:text-muted-foreground" />
              </label>
              <fieldset data-repo-webhook-events class="grid gap-1.5 rounded-md border border-border p-3 text-[11px] text-muted-foreground">
                <legend class="px-1 text-[11px] font-semibold uppercase tracking-wide">Events</legend>
              </fieldset>
              <div class="flex flex-wrap items-center justify-between gap-2">
                <span data-repo-webhook-status class="text-[11px] text-muted-foreground"></span>
                <button type="submit" class="inline-flex h-8 items-center rounded-md bg-cta px-3 text-xs font-medium text-cta-foreground hover:bg-cta/90">Add webhook</button>
              </div>
            </form>
            <div data-repo-webhook-secret hidden class="rounded-md border border-border bg-secondary/40 p-3 text-[11px] leading-5 text-foreground">
              <p class="font-semibold">Signing secret — shown once</p>
              <p class="mt-1 text-muted-foreground">Store it on the receiving side now. ${escapeHtml(platformName())} keeps it encrypted and never shows it again; if it is lost, rotate the webhook.</p>
              <code data-repo-webhook-secret-value class="mt-2 block break-all rounded bg-background p-2 font-mono text-[11px]"></code>
            </div>
          </div>
        </div>
        <div data-repo-actions-secrets-card class="mt-5 overflow-hidden rounded-lg border border-border bg-background">
          <div class="flex flex-wrap items-center justify-between gap-2 border-b border-border bg-secondary/50 px-4 py-3">
            <span class="inline-flex items-center gap-2 text-xs font-medium text-foreground"><i data-lucide="key-round" class="h-3.5 w-3.5 text-muted-foreground"></i>Actions secrets</span>
            <span class="font-mono text-[10px] text-muted-foreground">secrets.NAME</span>
          </div>
          <div data-actions-secrets data-actions-secrets-scope="repo" data-actions-secrets-base="${escapeHtml(`${repoApiBase(repo)}/actions/secrets`)}" class="p-4 text-xs text-muted-foreground"></div>
        </div>
        <div data-repo-forge-mirror-card id="mirror" class="mt-5 scroll-mt-20 overflow-hidden rounded-lg border border-border bg-background">
          <div class="flex flex-wrap items-center justify-between gap-2 border-b border-border bg-secondary/50 px-4 py-3">
            <span class="inline-flex items-center gap-2 text-xs font-medium text-foreground"><i data-lucide="arrow-left-right" class="h-3.5 w-3.5 text-muted-foreground"></i>Mirror to GitHub or GitLab</span>
            <span data-repo-forge-mirror-summary class="inline-flex items-center gap-1.5 text-[11px] text-muted-foreground">Loading…</span>
          </div>
          <div class="grid gap-4 p-4">
            <p class="text-[11px] leading-5 text-muted-foreground">Keep a copy of this repository on GitHub or GitLab with your own access token. Choose, for code, issues, pull requests, discussions and releases, whether each one is sent from ${escapeHtml(platformName())}, brought in from the other side, both, or neither, and whether imports happen on their own or only for the items you pick.</p>
            <div data-repo-forge-mirror-body class="text-xs text-muted-foreground">Loading mirror settings…</div>
            <span data-repo-forge-mirror-status class="text-[11px] text-muted-foreground"></span>
          </div>
        </div>
        <div data-repo-danger-zone class="mt-5 overflow-hidden rounded-lg border border-destructive/50 bg-background">
          <div class="border-b border-destructive/30 px-4 py-3">
            <h3 class="text-sm font-semibold text-destructive">Danger zone</h3>
          </div>
          <div class="flex flex-col gap-3 p-4 sm:flex-row sm:items-center sm:justify-between">
            <div>
              <p class="text-sm font-medium text-foreground">Delete this repository</p>
              <p class="mt-1 text-xs leading-5 text-muted-foreground">Removes the repository from ${escapeHtml(platformName())}, including relay metadata, pending inboxes, agents, mirror presence, and repository chat history. Source files on the owner's device are not erased.</p>
              <p data-repo-delete-status class="mt-1 text-xs text-muted-foreground"></p>
            </div>
            <button type="button" data-repo-delete class="inline-flex h-8 shrink-0 items-center justify-center gap-1.5 rounded-md border border-destructive/60 px-3 text-xs font-semibold text-destructive hover:bg-destructive/10"><i data-lucide="trash-2" class="h-3.5 w-3.5"></i>Delete repository</button>
          </div>
        </div>
      </section>` : "";
    keepRepoFirstViewIssue(detail);
    detail.innerHTML = `
      <div data-repo-layout="github-like" class="min-w-0">
        <div data-repo-github-header class="border-b border-border">
          <div class="flex w-full min-w-0 gap-1 overflow-x-auto px-3 sm:px-6" role="tablist" aria-label="Repository sections">
            ${["code", "issues", "pulls", "discussions", "insights", "projects", "actions", "security", ...(canSeeAgentsTab ? ["agents"] : []), ...(canSeeSettingsTab ? ["settings"] : [])].map((tab) => {
              const meta = tabMeta[tab];
              const isDiscussions = tab === "discussions";
              const discussionCount = Number(meta.count);
              // Discussion counts are catalog snapshots, while discussion rows
              // come from a live mirror. Do not present the snapshot as
              // currently available when the catalog says every mirror is
              // offline; loadRepoCollection will restore the badge if its
              // recovery attempt reaches one.
              const hideDiscussionsBadge = isDiscussions && (
                !repoIsLive(repo) || !Number.isFinite(discussionCount) || discussionCount <= 0
              );
              const iconAttr = tab === "issues"
                ? 'data-lucide="circle-dot"'
                : tab === "pulls"
                  ? 'data-lucide="git-pull-request"'
                  : `data-lucide="${meta.icon}"`;
              const showTabCount = isDiscussions || meta.count !== "";
              // GitHub's tab strip: white 14px labels, grey 16px icons, a
              // counter pill, and a hover wash on the inner row. The selected
              // tab's coral underline comes from aria-selected in shell.html.
              // Each tab is a real link to its own page so middle-click and
              // ctrl/cmd-click open it in a new browser tab; a plain click is
              // routed in-app by the document click handler.
              const tabHref = tab === "code" ? repoLinkUrl(repo) : `${repoLinkUrl(repo)}/${tab}`;
              const pluginEntry = { code: "code", issues: "issues", pulls: "pull-requests", discussions: "discussions", actions: "actions", agents: "agents" }[tab];
              return `<a href="${escapeHtml(tabHref)}" role="tab" data-dashboard-repo-tab="${tab}"${pluginEntry ? ` data-plugin-entry="${pluginEntry}"` : ""} aria-selected="${tab === "code" ? "true" : "false"}" class="group relative inline-flex h-12 shrink-0 items-center px-1 text-[14px] font-normal text-foreground"><span class="inline-flex h-8 items-center gap-2 rounded-md px-2 transition-colors group-hover:bg-secondary"><i ${iconAttr} class="h-4 w-4 text-muted-foreground"></i><span>${meta.label}</span>${showTabCount ? `<span data-dashboard-repo-tab-count="${tab}" class="min-w-[20px] rounded-full bg-[var(--dashboard-counter)] px-1.5 text-center text-[12px] font-medium leading-[18px] text-foreground${hideDiscussionsBadge ? " hidden" : ""}">${tabCountLabel(meta.count)}</span>` : ""}</span></a>`;
            }).join("")}
          </div>
        </div>
	        <div data-repo-content-grid class="mx-auto grid w-full max-w-7xl min-w-0 gap-5 px-3 pb-3 pt-4 sm:p-5 lg:grid-cols-[minmax(0,1fr)_18rem]">
		          <div class="min-w-0">
		            <section data-dashboard-repo-tab-panel="code">
		              <div data-repo-root-toolbar class="grid gap-2 md:grid-cols-[auto_minmax(0,1fr)_auto_auto_auto]">
		                ${renderRepoBranchToolbar(repo, branch)}
		                <button type="button" data-repo-file-finder-open class="inline-flex h-9 min-w-0 items-center gap-2 rounded-md border border-border bg-background px-3 text-left text-xs text-muted-foreground hover:bg-secondary hover:text-foreground transition-colors"><i data-lucide="search" class="h-3.5 w-3.5 shrink-0"></i><span class="min-w-0 truncate">Go to file</span><span class="ml-auto hidden rounded border border-border px-1.5 py-0.5 font-mono text-[10px] text-muted-foreground sm:inline">T</span></button>
		                <button type="button" data-repo-codemap-open title="See the whole codebase as a zoomable treemap, in 2D or 3D" class="inline-flex h-9 items-center justify-center gap-2 rounded-md border border-border bg-secondary px-3 text-xs font-semibold text-foreground hover:bg-background"><i data-lucide="layout-grid" class="h-3.5 w-3.5 text-muted-foreground"></i><span data-repo-codemap-label>Code map</span></button>
		                <button type="button" aria-disabled="true" class="inline-flex h-9 items-center justify-center gap-2 rounded-md border border-border bg-secondary px-3 text-xs font-semibold text-foreground hover:bg-background"><i data-lucide="plus" class="h-3.5 w-3.5 text-muted-foreground"></i>Add file<i data-lucide="chevron-down" class="h-3 w-3 text-muted-foreground"></i></button>
		                <div class="flex min-w-0 items-center gap-2">${mirrorButton(false)}${renderRepoCodeButton(repo, false)}</div>
		              </div>
		              <div data-repo-pathbar class="my-3 flex min-w-0 flex-col gap-2 md:flex-row md:items-center md:justify-between">
	                <div class="flex min-w-0 items-center gap-2"><img data-repo-dir-logo hidden alt="" width="20" height="20" decoding="async" referrerpolicy="no-referrer" class="h-5 w-5 shrink-0 rounded object-contain" /><div class="min-w-0 truncate text-xs text-muted-foreground" data-repo-breadcrumb></div></div>
		                <div data-repo-focus-actions class="hidden flex shrink-0 flex-wrap items-center gap-2">
		                  <button type="button" data-repo-file-finder-open class="inline-flex h-8 min-w-0 items-center gap-2 rounded-md border border-border bg-background px-3 text-left text-xs text-muted-foreground hover:bg-secondary hover:text-foreground transition-colors"><i data-lucide="search" class="h-3.5 w-3.5 shrink-0"></i><span class="min-w-0 truncate">Go to file</span><span class="ml-auto rounded border border-border px-1.5 py-0.5 font-mono text-[10px] text-muted-foreground">T</span></button>
		                  <button type="button" data-repo-codemap-open title="See the whole codebase as a zoomable treemap, in 2D or 3D" class="inline-flex h-8 items-center justify-center gap-2 rounded-md border border-border bg-secondary px-3 text-xs font-semibold text-foreground hover:bg-background"><i data-lucide="layout-grid" class="h-3.5 w-3.5 text-muted-foreground"></i><span data-repo-codemap-label>Code map</span></button>
		                  <button type="button" aria-disabled="true" class="inline-flex h-8 items-center justify-center gap-2 rounded-md border border-border bg-secondary px-3 text-xs font-semibold text-foreground hover:bg-background"><i data-lucide="plus" class="h-3.5 w-3.5 text-muted-foreground"></i>Add file</button>
		                  ${mirrorButton(true)}
		                  ${renderRepoCodeButton(repo, true)}
		                </div>
		              </div>
		              <div data-repo-code-workspace class="min-w-0 gap-4">
		                <aside data-repo-code-explorer class="hidden min-w-0 overflow-hidden rounded-lg border border-border bg-background" data-repo-code-sidebar>
		                  <div class="flex h-11 items-center gap-2 border-b border-border bg-secondary/40 px-3 text-sm font-semibold text-foreground">
		                    <i data-lucide="panel-left" class="h-3.5 w-3.5 text-muted-foreground"></i>
		                    Files
	                  </div>
	                  <div class="grid gap-2 border-b border-border p-3">
	                    ${renderRepoBranchToolbar(repo, branch)}
	                    <button type="button" data-repo-file-finder-open class="inline-flex h-8 min-w-0 items-center gap-2 rounded-md border border-border bg-background px-3 text-left text-xs text-muted-foreground hover:bg-secondary hover:text-foreground transition-colors"><i data-lucide="search" class="h-3.5 w-3.5 shrink-0"></i><span class="min-w-0 truncate">Go to file</span><span class="ml-auto rounded border border-border px-1.5 py-0.5 font-mono text-[10px] text-muted-foreground">T</span></button>
		                  </div>
		                  <div data-repo-explorer-tree class="max-h-[35rem] overflow-auto py-2"></div>
		                </aside>
		                <div data-repo-code-main class="min-w-0">
		                  ${(() => {
		                    const provenance = repoProvenanceHtml(repo);
		                    return `<div data-repo-provenance class="${provenance ? "" : "hidden "}mb-2 flex min-w-0 flex-wrap items-center gap-x-3 gap-y-1 text-xs text-muted-foreground">${provenance}</div>`;
		                  })()}
		                  <div data-repo-tree-panel data-repo-file-table class="overflow-hidden rounded-lg border border-border bg-background">
	                    <div data-repo-commit-summary class="grid gap-2 border-b border-border bg-secondary/50 px-4 py-3 text-xs sm:grid-cols-[minmax(0,1fr)_auto_auto_auto] sm:items-center">
	                      <div class="flex min-w-0 items-center gap-2">
	                        <span data-repo-commit-avatar class="flex h-6 w-6 shrink-0 items-center justify-center rounded-full border border-border/30 bg-primary/10 font-mono text-[10px] font-semibold text-muted-foreground">${escapeHtml((repo.owner || "F")[0] || "F").toUpperCase()}</span>
	                        <span data-repo-commit-author class="min-w-0 truncate text-foreground font-medium"><span class="inline-block h-3.5 w-24 max-w-full animate-pulse rounded bg-muted-foreground/20 align-middle"></span></span>
	                        <span data-repo-commit-message class="min-w-0 truncate text-muted-foreground"><span class="inline-block h-3.5 w-40 max-w-full animate-pulse rounded bg-muted-foreground/20 align-middle"></span></span>
	                      </div>
	                      <span data-repo-commit-hash class="font-mono text-muted-foreground"><span class="inline-block h-3.5 w-14 animate-pulse rounded bg-muted-foreground/20 align-middle"></span></span>
	                      <span data-repo-commit-date class="font-mono text-muted-foreground"><span class="inline-block h-3.5 w-16 animate-pulse rounded bg-muted-foreground/20 align-middle"></span></span>
	                      <a href="${escapeHtml(`${repoLinkUrl(repo)}/commits`)}" data-dashboard-history-button data-plugin-entry="commits" aria-label="Open commit history" class="inline-flex items-center gap-1 font-medium text-foreground transition-colors hover:text-accent"><i data-lucide="history" class="h-3.5 w-3.5 text-muted-foreground"></i><span>History</span><span data-dashboard-repo-count="commits" data-dashboard-repo-tab-count="commits" class="rounded-full bg-background px-1.5 py-0.5 font-mono text-[10px] font-normal text-muted-foreground">${tabCountLabel(commitsCount)}</span></a>
	                    </div>
	                    <div data-repo-tree></div>
	                  </div>
	                  <div data-repo-blob class="hidden"></div>
	                  <section data-repo-readme class="mt-4 overflow-hidden rounded-lg border border-border bg-background">
	                    <div data-repo-readme-filename class="flex items-center gap-2 border-b border-border bg-secondary/50 px-4 py-3 text-xs font-medium text-foreground"><i data-lucide="book-open" class="h-3.5 w-3.5 text-muted-foreground"></i>README.md</div>
	                    <div data-repo-readme-body class="p-4 text-sm leading-6 text-muted-foreground">
	                      <p class="mt-1">${loadingHtml("Loading README...")}</p>
	                    </div>
	                  </section>
	                </div>
	              </div>
	              <div data-repo-file-finder class="fixed inset-0 z-50 hidden items-start justify-center bg-black/50 p-4 pt-20" role="dialog" aria-modal="true" aria-label="Go to file">
	                <button type="button" tabindex="-1" data-repo-file-finder-backdrop class="absolute inset-0 cursor-default" aria-label="Close file finder"></button>
	                <div data-repo-file-finder-dialog class="fm-popover relative z-10 grid w-full max-w-2xl overflow-hidden">
	                  <div class="flex items-center gap-3 border-b border-border px-4 py-3">
	                    <i data-lucide="search" class="h-4 w-4 text-muted-foreground"></i>
	                    <input data-repo-file-finder-input type="search" role="combobox" aria-autocomplete="list" aria-controls="repo-file-finder-results" aria-expanded="true" autocomplete="off" spellcheck="false" placeholder="Search files in this live mirror" class="min-w-0 flex-1 bg-transparent font-mono text-sm text-foreground outline-none placeholder:text-muted-foreground" />
	                    <button type="button" data-repo-file-finder-close aria-label="Close file finder" class="fm-btn fm-btn-sm">Esc</button>
	                  </div>
	                  <div data-repo-file-finder-status role="status" aria-live="polite" class="border-b border-border px-4 py-2 text-xs text-muted-foreground">Type to search files.</div>
	                  <div id="repo-file-finder-results" data-repo-file-finder-results role="listbox" aria-label="Matching files" class="max-h-[26rem] overflow-auto p-2"></div>
	                </div>
	              </div>
		            </section>
            <section data-dashboard-repo-tab-panel="branches" class="hidden"><div class="mt-4 overflow-hidden rounded-lg border border-border bg-background"><div class="flex flex-wrap items-center justify-between gap-3 border-b border-border bg-secondary/50 px-4 py-3"><span class="inline-flex items-center gap-2 text-xs font-medium text-foreground"><i data-lucide="git-branch" class="h-3.5 w-3.5 text-muted-foreground"></i>Branches<span data-repo-branches-total class="font-mono text-[10px] font-normal text-muted-foreground"></span></span><label class="flex h-8 min-w-0 flex-1 items-center gap-2 rounded-md border border-border bg-background px-3 text-xs text-muted-foreground focus-within:ring-1 focus-within:ring-foreground/20 sm:max-w-64"><i data-lucide="search" class="h-3.5 w-3.5 shrink-0"></i><input data-repo-branches-search type="search" autocomplete="off" spellcheck="false" placeholder="Find a branch..." aria-label="Find a branch" class="min-w-0 flex-1 bg-transparent text-foreground outline-none placeholder:text-muted-foreground" /></label><div data-repo-branches-actions class="flex max-w-full flex-wrap items-center gap-2"></div></div><div data-repo-branches-status role="status" aria-live="polite" hidden class="border-b border-border px-4 py-2 text-xs text-muted-foreground"></div><div data-repo-branches-selection hidden></div><div data-repo-branch-delete-results hidden></div><div data-repo-branch-results hidden></div><div data-repo-branches></div></div></section>
            <section data-dashboard-repo-tab-panel="commits" class="hidden"><div class="mt-4 overflow-hidden rounded-lg border border-border bg-background"><div class="flex items-center justify-between gap-3 border-b border-border bg-secondary/50 px-4 py-3"><span class="inline-flex items-center gap-2 text-xs font-medium text-foreground"><i data-lucide="git-commit-horizontal" class="h-3.5 w-3.5 text-muted-foreground"></i>Commits</span><span class="font-mono text-[10px] text-muted-foreground">live mirror history</span></div><div data-repo-commits></div></div></section>
            <section data-dashboard-repo-tab-panel="releases" class="hidden"><div class="mt-4 overflow-hidden rounded-lg border border-border bg-background"><div class="flex items-center justify-between gap-3 border-b border-border bg-secondary/50 px-4 py-3"><span class="inline-flex items-center gap-2 text-xs font-medium text-foreground"><i data-lucide="tag" class="h-3.5 w-3.5 text-muted-foreground"></i>Releases</span><span class="font-mono text-[10px] text-muted-foreground">signed release manifests</span></div><div data-repo-releases></div></div></section>
            ${renderRepoCollectionPanel("issues", repo, issuesCount, repoCount(repo, ["closedIssues", "closedIssueCount"]))}
            <section data-dashboard-repo-tab-panel="projects" class="hidden"><div class="mt-4 overflow-hidden rounded-lg border border-border bg-background"><div class="flex items-center justify-between gap-3 border-b border-border bg-secondary/50 px-4 py-3"><span class="inline-flex items-center gap-2 text-xs font-medium text-foreground"><i data-lucide="chart-gantt" class="h-3.5 w-3.5 text-muted-foreground"></i>Projects</span><span class="font-mono text-[10px] text-muted-foreground">linked issues · milestones · gantt</span></div><div data-repo-projects></div></div></section>
            <section data-dashboard-repo-tab-panel="security" class="hidden"><div data-repo-security class="min-w-0"></div></section>
            ${renderRepoCollectionPanel("pulls", repo, pullsCount, repoCount(repo, ["closedPulls", "closedPullCount"]))}
            ${renderRepoDiscussionsPanel(repo)}
            <section data-dashboard-repo-tab-panel="actions" class="hidden"><div class="mt-4 overflow-hidden rounded-lg border border-border bg-background"><div class="flex items-center justify-between gap-3 border-b border-border bg-secondary/50 px-4 py-3"><span class="inline-flex items-center gap-2 text-xs font-medium text-foreground"><i data-lucide="workflow" class="h-3.5 w-3.5 text-muted-foreground"></i>Actions</span><span class="font-mono text-[10px] text-muted-foreground">workflows in .forkmesh/ · runs from the mirror executor</span></div><div data-repo-actions></div></div></section>
            <section data-dashboard-repo-tab-panel="insights" class="hidden"><div class="mt-4 overflow-hidden rounded-lg border border-border bg-background"><div class="flex items-center justify-between gap-3 border-b border-border bg-secondary/50 px-4 py-3"><span class="inline-flex min-w-0 items-start gap-2"><i data-lucide="chart-no-axes-combined" class="mt-0.5 h-3.5 w-3.5 shrink-0 text-muted-foreground"></i><span><span class="block text-xs font-medium text-foreground">Insights</span><span class="mt-0.5 block text-[10px] text-muted-foreground">Repository-wide code, collaboration, and contributor activity.</span></span></span><button type="button" data-repo-insights-refresh class="inline-flex h-7 shrink-0 items-center gap-1.5 rounded-md border border-border px-2.5 text-xs font-medium text-muted-foreground hover:bg-secondary hover:text-foreground"><i data-lucide="refresh-cw" class="h-3.5 w-3.5"></i>Refresh</button></div><div data-repo-insights></div></div></section>
            <section data-dashboard-repo-tab-panel="sizemap" class="hidden"><div class="mt-4 overflow-hidden rounded-lg border border-border bg-background"><div class="flex items-center justify-between gap-3 border-b border-border bg-secondary/50 px-4 py-3"><span class="inline-flex items-center gap-2 text-xs font-medium text-foreground"><i data-lucide="chart-pie" class="h-3.5 w-3.5 text-muted-foreground"></i>Size map</span><span class="font-mono text-[10px] text-muted-foreground">directory sizes · default branch</span></div><div data-repo-sizemap class="p-4"></div></div></section>
            <section data-dashboard-repo-tab-panel="mirrors" class="hidden"><div data-mirror-request hidden class="py-2"><label class="block text-[11px] font-medium text-foreground">Ask a node to mirror this repo</label><div class="flex items-center gap-2"><input data-mirror-request-target type="text" autocomplete="off" spellcheck="false" placeholder="node name" class="h-8 min-w-0 flex-1 rounded-md bg-secondary px-2 font-mono text-xs text-foreground outline-none placeholder:text-muted-foreground" /><button type="button" data-mirror-request-send class="h-8 shrink-0 rounded-md bg-secondary px-3 text-xs font-medium text-foreground transition-colors hover:bg-secondary/70">Ask to mirror</button></div><p data-mirror-request-hint class="text-[11px] text-muted-foreground">They get a ping; if they accept, their node starts mirroring your repo.</p></div><div data-repo-mirrors></div></section>
            ${canSeeAgentsTab ? `<section data-dashboard-repo-tab-panel="agents" class="hidden"><div class="mt-4 overflow-hidden rounded-lg border border-border bg-background"><div class="flex items-center justify-between gap-3 border-b border-border bg-secondary/50 px-4 py-3"><span class="inline-flex items-center gap-2 text-xs font-medium text-foreground"><i data-lucide="bot" class="h-3.5 w-3.5 text-muted-foreground"></i>Agents</span><button type="button" data-repo-agents-refresh class="inline-flex h-7 items-center gap-1.5 rounded-md border border-border px-2.5 text-xs font-medium text-muted-foreground hover:bg-secondary hover:text-foreground"><i data-lucide="refresh-cw" class="h-3.5 w-3.5"></i>Refresh</button></div><div data-repo-agents></div></div></section>` : ""}
            ${settingsPanel}
          </div>
          <aside data-repo-about data-repo-about-rail class="min-w-0 self-start rounded-lg border border-border bg-background p-4">
            <div class="flex items-center justify-between gap-3">
              <h3 class="text-sm font-semibold text-foreground">About</h3>
              ${canEditAbout
                ? `<button type="button" data-repo-about-edit aria-label="Edit About" class="inline-flex h-7 w-7 items-center justify-center rounded-md text-muted-foreground hover:bg-secondary hover:text-foreground"><i data-lucide="pencil" class="h-3.5 w-3.5"></i></button>`
                : ""}
            </div>
            <p data-repo-about-description class="mt-2 text-sm leading-6 text-foreground">${escapeHtml(repoDescriptionText(repo) || "No description published.")}</p>
            <div class="mt-2 flex flex-wrap gap-x-4 gap-y-1.5 text-xs text-muted-foreground">
              <a href="${escapeHtml(cloneUrl(repo))}" class="inline-flex min-w-0 items-center gap-1.5 hover:text-foreground"><i data-lucide="link" class="h-3.5 w-3.5 shrink-0"></i><span class="min-w-0 truncate">Open clean URL</span></a>
              <a href="${escapeHtml(readmeHref)}" data-repo-readme-link data-repo-readme-path="${escapeHtml(readmePath)}" class="inline-flex min-w-0 items-center gap-1.5 hover:text-foreground"><i data-lucide="book-open" class="h-3.5 w-3.5"></i><span>Readme</span></a>
              <a href="${escapeHtml(`${repoLinkUrl(repo)}/insights`)}" data-repo-activity-link class="inline-flex min-w-0 items-center gap-1.5 hover:text-foreground"><i data-lucide="activity" class="h-3.5 w-3.5"></i><span>Activity</span></a>
            </div>
            <form data-repo-about-form class="mt-3 hidden grid gap-2">
              <textarea data-repo-about-input rows="4" maxlength="240" class="min-h-24 rounded-md border border-border bg-background px-3 py-2 text-sm text-foreground outline-none focus:border-accent">${escapeHtml(repo.description || "")}</textarea>
              <label class="grid gap-1 text-[11px] text-muted-foreground">Website
                <input data-repo-about-website-input type="url" maxlength="240" placeholder="https://example.com" class="h-8 rounded-md border border-border bg-background px-3 text-sm text-foreground outline-none focus:border-accent" />
              </label>
              <label class="grid gap-1 text-[11px] text-muted-foreground">Logo — square PNG, up to 256 KB (fediverse avatar)
                <input data-repo-about-logo type="file" accept="image/png" class="text-xs text-muted-foreground file:mr-2 file:rounded-md file:border file:border-border file:bg-secondary file:px-2 file:py-1 file:text-xs file:text-foreground" />
              </label>
              <label class="grid gap-1 text-[11px] text-muted-foreground">Banner — 1500×500 PNG, up to 1 MB (fediverse header)
                <input data-repo-about-banner type="file" accept="image/png" class="text-xs text-muted-foreground file:mr-2 file:rounded-md file:border file:border-border file:bg-secondary file:px-2 file:py-1 file:text-xs file:text-foreground" />
                <span class="text-[10px] text-muted-foreground">Or commit <code class="font-mono">.forkmesh/banner.png</code> — the repo carries its own header, and every mirror serves it.</span>
              </label>
              <span class="flex flex-wrap gap-3 text-[11px] text-muted-foreground">
                <label class="inline-flex items-center gap-1.5"><input data-repo-about-logo-clear type="checkbox" class="h-3 w-3" />Remove logo</label>
                <label class="inline-flex items-center gap-1.5"><input data-repo-about-banner-clear type="checkbox" class="h-3 w-3" />Remove banner</label>
              </span>
              <div class="flex flex-wrap items-center justify-between gap-2">
                <span data-repo-about-status class="text-[11px] text-muted-foreground"></span>
                <span class="inline-flex items-center gap-2">
                  <button type="button" data-repo-about-cancel class="inline-flex h-8 items-center rounded-md border border-border px-3 text-xs font-medium text-muted-foreground hover:bg-secondary hover:text-foreground">Cancel</button>
                  <button type="submit" class="inline-flex h-8 items-center rounded-md bg-cta px-3 text-xs font-medium text-cta-foreground hover:bg-cta/90">Save</button>
                </span>
              </div>
            </form>
            <div data-repo-about-actions aria-label="Repository details" class="-mx-2 mt-3 grid">
              <a data-repo-about-website href="#" target="_blank" rel="noopener noreferrer" class="hidden grid grid-cols-[auto_minmax(0,1fr)] items-center gap-3 w-full rounded-md px-2 py-1.5 text-left text-xs transition-colors hover:bg-secondary/60"><span class="shrink-0 text-muted-foreground">Website</span><span data-repo-about-website-label class="dashboard-accent-link min-w-0 truncate text-right"></span></a>
              <a href="${escapeHtml(`${repoLinkUrl(repo)}/releases`)}" data-repo-about-release data-dashboard-releases-button data-plugin-entry="releases" class="hidden grid grid-cols-[auto_minmax(0,1fr)] items-center gap-3 w-full rounded-md px-2 py-1.5 text-left text-xs transition-colors hover:bg-secondary/60"><span class="shrink-0 text-muted-foreground">Release</span><span data-repo-about-release-body class="min-w-0 truncate text-right text-muted-foreground"></span></a>
              <button type="button" data-repo-action-star data-repo-key="${escapeHtml(actionSeed)}" aria-pressed="false" aria-label="Star repository" title="Star this repository" class="grid grid-cols-[auto_minmax(0,1fr)] items-center gap-3 w-full rounded-md px-2 py-1.5 text-left text-xs transition-colors hover:bg-secondary/60"><span class="shrink-0 text-muted-foreground">Stars</span><span class="inline-flex items-center gap-1.5 justify-self-end text-foreground"><span data-repo-star-label class="sr-only">Star</span><i data-lucide="star" data-repo-star-icon class="h-3.5 w-3.5 text-muted-foreground"></i><span data-repo-star-count class="font-mono">${formatCount(0)}</span></span></button>
              <div data-repo-watch-wrap class="relative min-w-0">
                <button type="button" data-repo-action-watch aria-label="Watch repository" class="grid grid-cols-[auto_minmax(0,1fr)] items-center gap-3 w-full rounded-md px-2 py-1.5 text-left text-xs transition-colors hover:bg-secondary/60"><span class="shrink-0 text-muted-foreground">Watchers</span><span class="inline-flex items-center gap-1 justify-self-end text-foreground"><span data-repo-watch-count class="font-mono">–</span><i data-lucide="chevron-down" class="h-3 w-3 text-muted-foreground"></i></span></button>
                <div data-repo-watch-menu class="fm-popover absolute right-0 top-full z-30 mt-1 hidden w-64 max-w-[calc(100vw-2rem)] p-3 text-left">
                  <p class="text-xs font-semibold text-foreground">Watch on the fediverse</p>
                  <p class="mt-1 text-[11px] leading-5 text-muted-foreground">Follow this repository from Mastodon (or any ActivityPub app) to get new issues, pull requests, discussions and releases in your feed.</p>
                  <div class="mt-2 flex items-center gap-2">
                    <code data-repo-watch-handle class="min-w-0 flex-1 truncate rounded-md border border-border bg-secondary px-2 py-1 font-mono text-[11px] text-foreground">${escapeHtml(fediHandle)}</code>
                    <button type="button" data-dashboard-copy="${escapeHtml(fediHandle)}" aria-label="Copy fediverse handle" class="copy-button inline-flex h-7 w-7 shrink-0 items-center justify-center rounded-md border border-border text-muted-foreground hover:bg-secondary hover:text-foreground"><i data-lucide="copy" class="copy-icon h-3.5 w-3.5"></i><i data-lucide="check" class="copy-check h-3.5 w-3.5"></i></button>
                  </div>
                  <p class="mt-2 text-[11px] text-muted-foreground"><span data-repo-watch-followers class="min-w-0 truncate text-right font-mono text-foreground">–</span> fediverse watchers</p>
                  <p data-repo-watch-disabled class="mt-1 hidden text-[11px] text-attention">Federation is turned off for this repository.</p>
                  <div data-repo-watch-list class="mt-2 hidden max-h-44 overflow-auto rounded-md border border-border bg-secondary/40 p-2"></div>
                  <a data-repo-mastodon-link href="${escapeHtml(mastodonUrl)}" target="_blank" rel="noopener noreferrer" class="dashboard-accent-link mt-2 inline-flex items-center gap-1.5 text-[11px] hover:underline"><i data-lucide="external-link" class="h-3.5 w-3.5 shrink-0"></i>View on Mastodon</a>
                </div>
              </div>
              <div data-repo-fork-wrap class="relative min-w-0">
                <button type="button" data-repo-action-fork aria-haspopup="true" aria-expanded="false" aria-label="Show who forked this repository" class="grid grid-cols-[auto_minmax(0,1fr)] items-center gap-3 w-full rounded-md px-2 py-1.5 text-left text-xs transition-colors hover:bg-secondary/60"><span class="shrink-0 text-muted-foreground">Forks</span><span class="inline-flex items-center gap-1 justify-self-end text-foreground"><span data-repo-fork-count class="font-mono">–</span><i data-lucide="chevron-down" class="h-3 w-3 text-muted-foreground"></i></span></button>
                <div data-repo-fork-menu class="fm-popover absolute right-0 top-full z-30 mt-1 hidden w-72 max-w-[calc(100vw-2rem)] p-3 text-left">
                  <p class="text-xs font-semibold text-foreground">Forks of this repository</p>
                  <p class="mt-1 text-[11px] leading-5 text-muted-foreground">Each fork is its own repository on the mesh, sharing this one's history instead of copying it.</p>
                  ${websiteForkAvailable ? `<button type="button" data-repo-fork-create class="fm-btn fm-btn-sm fm-btn-cta mt-2 w-full disabled:cursor-wait"><i data-lucide="git-fork"></i>Fork this repository</button><p data-repo-fork-status class="mt-1 text-[10px] leading-4 text-muted-foreground">Your own copy under your account, linked to this one. Push any branch to it, then open a pull request back here.</p>` : ""}
                  <div data-repo-fork-list class="mt-2 max-h-52 space-y-1 overflow-auto rounded-md border border-border bg-secondary/40 p-2 text-[11px] text-muted-foreground">Loading…</div>
                  <div data-repo-remote-forks class="mt-3 border-t border-border pt-2">
                    <p class="text-xs font-semibold text-foreground">Forks on other instances</p>
                    <p class="mt-1 text-[11px] leading-5 text-muted-foreground">Copies of this repository on other ForkMesh instances. You can send pull requests to them.</p>
                    <div data-repo-remote-fork-list class="mt-2 max-h-40 space-y-1 overflow-auto rounded-md border border-border bg-secondary/40 p-2 text-[11px] text-muted-foreground">Loading…</div>
                    <form data-repo-remote-fork-form hidden class="mt-2 grid gap-1.5">
                      <div class="flex gap-1.5">
                        <input data-repo-remote-fork-url type="url" required inputmode="url" autocomplete="off" spellcheck="false" placeholder="https://forkmesh.com/owner/repository" aria-label="Address of the repository on another instance" class="h-8 min-w-0 flex-1 rounded-md border border-border bg-background px-2 text-[11px] text-foreground outline-none focus:border-accent" />
                        <button type="submit" class="fm-btn fm-btn-sm shrink-0"><i data-lucide="plus"></i>Save</button>
                      </div>
                      <p data-repo-remote-fork-status aria-live="polite" class="text-[10px] leading-4 text-muted-foreground"></p>
                    </form>
                  </div>
                </div>
              </div>
              <a href="${escapeHtml(`${repoLinkUrl(repo)}/mirrors`)}" data-dashboard-repo-tab="mirrors" aria-label="Show repository mirror status" class="grid grid-cols-[auto_minmax(0,1fr)] items-center gap-3 w-full rounded-md px-2 py-1.5 text-left text-xs transition-colors hover:bg-secondary/60"><span class="shrink-0 text-muted-foreground">Mirrors</span><span data-dashboard-repo-count="mirrors" data-dashboard-repo-tab-count="mirrors" class="min-w-0 truncate text-right font-mono text-foreground">${tabCountLabel(mirrorsCount)}</span></a>
              <div data-repo-about-files class="grid grid-cols-[auto_minmax(0,1fr)] items-center gap-3 w-full rounded-md px-2 py-1.5 text-left text-xs"><span class="shrink-0 text-muted-foreground">Files</span><span data-repo-about-files-count class="min-w-0 truncate text-right font-mono text-foreground">–</span></div>
              <div data-repo-about-sizemap>
                <button type="button" data-repo-about-sizemap-open title="Explore the repository size map" aria-label="Explore the repository size map" class="grid grid-cols-[auto_minmax(0,1fr)] items-center gap-3 w-full rounded-md px-2 py-1.5 text-left text-xs transition-colors hover:bg-secondary/60"><span class="shrink-0 text-muted-foreground">Size</span><span class="inline-flex items-center gap-1.5 justify-self-end text-foreground"><span data-repo-about-sizemap-chart class="block h-4 w-4"></span><span data-repo-about-sizemap-meta class="font-mono">–</span><i data-lucide="chevron-right" class="h-3 w-3 text-muted-foreground"></i></span></button>
              </div>
            </div>
            <div data-repo-about-langs data-repo-about-pending class="mt-3 border-t border-border pt-3">
              <h4 class="text-xs font-semibold text-foreground">Languages</h4>
              <div data-repo-about-langs-bar class="mt-2.5 flex h-1.5 w-full gap-0.5 overflow-hidden rounded-full"><span class="h-full w-full rounded-full bg-muted-foreground/20"></span></div>
              <div data-repo-about-langs-legend class="mt-2.5 grid grid-cols-2 gap-x-3 gap-y-1 text-[11px] text-muted-foreground">${'<span class="block rounded bg-muted-foreground/20">&nbsp;</span>'.repeat(6)}</div>
            </div>
            <div data-repo-about-contribs data-repo-about-pending class="mt-3 border-t border-border pt-3">
              <div class="flex items-center justify-between gap-3">
                <h4 class="text-xs font-semibold text-foreground">Contributors</h4>
                <span data-repo-about-contribs-count class="font-mono text-[11px] text-muted-foreground">–</span>
              </div>
              <div data-repo-about-contribs-list class="mt-2.5 flex items-center -space-x-1.5">${'<span class="h-7 w-7 rounded-full border-2 border-background bg-secondary"></span>'.repeat(8)}</div>
            </div>
            ${dashboardOnInstance() ? "" : `<div data-repo-live-summary class="mt-3 border-t border-border pt-3">
              <div class="flex items-center justify-between gap-3">
                <h4 class="text-xs font-semibold text-foreground">Direct access</h4>
                <span class="font-mono text-[11px] text-muted-foreground"><span data-dashboard-repo-count="mirrors">${tabCountLabel(mirrorsCount)}</span> online</span>
              </div>
              <p class="mt-1 text-[11px] leading-4 text-muted-foreground">If the controller is unavailable, open this repository on any listed public mirror.</p>
              <div data-repo-live-mirror-list class="-mx-2 mt-2 grid"></div>
            </div>`}
            ${repo.isPrivate ? "" : `
            <details data-repo-logo-workflow class="mt-4 border-t border-border pt-4">
              <summary class="flex cursor-pointer list-none items-center justify-between gap-2 text-xs font-semibold text-foreground">
                <span class="inline-flex items-center gap-1.5"><i data-lucide="image-plus" class="h-3.5 w-3.5 text-muted-foreground"></i>Repository logo</span>
                <i data-lucide="chevron-down" class="h-3.5 w-3.5 text-muted-foreground"></i>
              </summary>
              <p class="mt-2 text-[11px] leading-4 text-muted-foreground">${escapeHtml(platformName())} shows the logo committed at the root of the repository (logo.png), and never invents artwork for a project. Signed-in community members may suggest PNG, JPEG, or WebP artwork; the repository owner or a moderator must approve it before it becomes official.</p>
              <form data-repo-logo-suggestion-form class="mt-3 grid gap-2">
                <input data-repo-logo-suggestion-file type="file" accept="image/png,image/jpeg,image/webp" required class="text-xs text-muted-foreground file:mr-2 file:rounded-md file:border file:border-border file:bg-secondary file:px-2 file:py-1 file:text-xs file:text-foreground" />
                <input data-repo-logo-suggestion-attribution type="text" maxlength="240" placeholder="Artwork attribution (optional)" class="h-8 rounded-md border border-border bg-background px-2 text-xs text-foreground outline-none focus:border-accent" />
                <label class="inline-flex items-start gap-1.5 text-[11px] text-muted-foreground"><input data-repo-logo-suggestion-rights type="checkbox" required class="mt-0.5 h-3 w-3" />I created this image or have permission to submit it.</label>
                <label class="inline-flex items-start gap-1.5 text-[11px] text-muted-foreground"><input data-repo-logo-suggestion-ai type="checkbox" class="mt-0.5 h-3 w-3" />This image was AI-generated.</label>
                <div class="flex items-center justify-between gap-2">
                  <span data-repo-logo-suggestion-status class="text-[11px] text-muted-foreground"></span>
                  <button type="submit" class="h-8 rounded-md border border-border bg-secondary px-3 text-xs font-medium text-foreground hover:bg-secondary/70">Suggest logo</button>
                </div>
              </form>
              <div data-repo-logo-suggestions class="mt-3 grid gap-2"></div>
            </details>`}
            ${canEditAbout && !repo.isPrivate ? `
            <details data-repo-fedi-posts class="mt-4 border-t border-border pt-4">
              <summary class="flex cursor-pointer list-none items-center justify-between gap-2 text-xs font-semibold text-foreground"><span class="inline-flex items-center gap-1.5"><i data-lucide="megaphone" class="h-3.5 w-3.5 text-muted-foreground"></i>Fediverse posts</span><i data-lucide="chevron-down" class="h-3.5 w-3.5 shrink-0 text-muted-foreground transition-transform"></i></summary>
              <p class="mt-2 text-[11px] leading-4 text-muted-foreground">Posts this repository published to its Mastodon followers. Deleting one sends a removal to every follower's server so it disappears from their timelines.</p>
              <div data-repo-fedi-posts-list class="mt-2 grid gap-2"></div>
            </details>` : ""}
          </aside>
          <aside data-repo-dir-about aria-label="Folder details" class="hidden min-w-0 self-start rounded-lg border border-border bg-background p-4"></aside>
        </div>
      </div>`;
    applyRepoFirstViewAbout(repo);
    // Restore whichever tab the URL points at (e.g. a refresh on
    // /owner/repo/issues) instead of always defaulting back to Code. This
    // runs right after painting the DOM, before anything else that could
    // throw (icon rendering, feature-panel loads) — otherwise a later error
    // would leave the page stuck showing Code even though the URL (and the
    // markup underneath) is already on the right tab.
    const initialTab = compareRoute
      ? "pulls"
      : repoTabRoutesFor(repo).includes(routeKind) ? routeKind : "code";
    setRepoTab(initialTab);
    window.lucide?.createIcons();
    // Only fetch code when Code is visible. Warming the tree/README behind an
    // Issues or Pulls deep link multiplied cold-cache mirror traffic and made
    // the requested panel compete with data the visitor could not see.
    if (initialTab === "code") {
      state.loadedRepoTabs.code = true;
      if (routeKind === "blob" && routePath) loadRepositoryBlob(repo, routePath);
      else loadRepositoryTree(repo, routeKind === "tree" ? routePath : "");
      scheduleRepoAboutRail(repo);
    }
    // Feature tabs read only their own record folders, while code deep links
    // read only the requested subpath/blob. Neither response carries the
    // mirror's root counters, so refresh all header badges from the live root
    // instead of leaving unrelated tabs on stale catalog seeds (for example,
    // opening Issues used to update Issues while Pull requests stayed stale).
    // The bare Code view already has a root-tree request in flight above.
    if (initialTab !== "code" || ((routeKind === "tree" || routeKind === "blob") && routePath)) {
      refreshServedCounts(repo);
    }
    loadRepoFeaturePanels(repo, recordRoute, compareRoute, commitHash);
    if (initialTab === "security") void loadRepoSecurity(repo);
    loadRepoStarState(repo, $("[data-repo-action-star]"));
    loadRepoForks(repo);
  }

  // Shows one section of an open pull detail (conversation | commits | checks |
  // files | badge): the panels are isolated, matching the GitHub review surface, so
  // only the selected one is visible at a time. The section is part of the
  // record's URL, so this runs both from a tab click and when Back/Forward
  // steps between two sections of the record already on screen.
  function setRepoRecordSection(section, root = null) {
    const startedAt = interfaceNow();
    const tab = normalizeRepoRecordSection(section);
    state.repoRecordSection = tab;
    const article = root || $("[data-repo-record-detail]");
    if (!article) return;
    article.querySelectorAll("[data-repo-record-tab]").forEach((button) => {
      const active = button.dataset.repoRecordTab === tab;
      button.setAttribute("aria-selected", active ? "true" : "false");
    });
    article.querySelectorAll("[data-repo-record-panel]").forEach((panel) => {
      panel.classList.toggle(
        "hidden",
        panel.dataset.repoRecordPanel !== tab,
      );
    });
    const layout = article.querySelector("[data-repo-record-layout]");
    const sidebar = article.querySelector("[data-repo-record-sidebar]");
    const conversation = tab === "conversation";
    layout?.classList.toggle("lg:grid-cols-[minmax(0,1fr)_18rem]", conversation);
    layout?.classList.toggle("lg:grid-cols-1", !conversation);
    sidebar?.classList.toggle("hidden", !conversation);
    recordInterfaceMetric("navigation", startedAt);
  }

  // GitHub-style "Code" button: a green trigger that opens a popover with the
  // clone info instead of silently copying on click. Shows the HTTPS clone URL
  // (readonly, selectable, with a copy button) and the ready-to-paste
  // `git clone` command. Used both in the code toolbar and the compact
  // scroll-pinned focus bar (compact=true), each self-contained in its own
  // relative wrapper so the toggle handler can scope to the clicked one.
  function renderRepoCodeButton(repo, compact = false) {
    const url = cloneUrl(repo);
    const gitCmd = `git clone ${url}`;
    const sshFullUrl = String(repo?.sshUrl || "").trim();
    // ssh://git@host/owner/repo.git on the standard port reads as the
    // familiar git@host:owner/repo.git; both address the same repository.
    const scpMatch = /^ssh:\/\/git@([^/:\[\]]+)\/(.+)$/.exec(sshFullUrl);
    const sshUrl = scpMatch ? `git@${scpMatch[1]}:${scpMatch[2]}` : sshFullUrl;
    const sshCloneCmd = sshUrl ? `git clone ${sshUrl}` : "";
    const sshPushCmd = sshUrl ? `git remote set-url --push origin ${sshUrl}` : "";
    const btnHeight = compact ? "h-8" : "h-9";
    const copyBtn = (value, label) =>
      `<button type="button" data-dashboard-copy="${escapeHtml(value)}" aria-label="${label}" class="copy-button inline-flex h-7 w-7 shrink-0 items-center justify-center rounded-md border border-border text-muted-foreground hover:bg-secondary hover:text-foreground"><i data-lucide="copy" class="copy-icon h-3.5 w-3.5"></i><i data-lucide="check" class="copy-check h-3.5 w-3.5"></i></button>`;
    return `
      <div data-repo-code-wrap class="relative">
        <button type="button" data-repo-code-button aria-haspopup="true" aria-expanded="false" class="inline-flex ${btnHeight} items-center justify-center gap-2 rounded-md border border-foreground/10 bg-cta px-3 text-xs font-semibold text-cta-foreground hover:bg-cta/90 transition-colors">
          <i data-lucide="code" class="h-3.5 w-3.5"></i><span>Code</span><i data-lucide="chevron-down" class="h-3 w-3"></i>
        </button>
        <div data-repo-code-menu class="fm-popover absolute right-0 z-40 mt-1 hidden w-96 max-w-[calc(100vw-2rem)] p-3 text-left">
          <div class="flex items-center justify-between gap-2">
            <span class="inline-flex items-center gap-1.5 text-xs font-semibold text-foreground"><i data-lucide="terminal" class="h-3.5 w-3.5 text-muted-foreground"></i>Clone</span>
            <span class="fm-label">HTTPS</span>
          </div>
          <p class="mt-1 text-[11px] leading-4 text-muted-foreground">Clone this repository from its live ${escapeHtml(platformName())} mirror over HTTPS.</p>
          <div class="mt-2 flex items-center gap-2">
            <input data-repo-clone-url type="text" readonly value="${escapeHtml(url)}" aria-label="Clone URL" class="min-w-0 flex-1 rounded-md border border-border bg-secondary px-2 py-1 font-mono text-[11px] text-foreground outline-none focus:border-accent" />
            ${copyBtn(url, "Copy clone URL")}
          </div>
          <div class="mt-3 border-t border-border pt-2">
            <span class="text-[11px] font-medium text-muted-foreground">Command line</span>
            <div class="mt-1 flex items-center gap-2">
              <code class="min-w-0 flex-1 truncate rounded-md border border-border bg-secondary px-2 py-1 font-mono text-[11px] text-foreground">${escapeHtml(gitCmd)}</code>
              ${copyBtn(gitCmd, "Copy git clone command")}
            </div>
          </div>
          ${sshUrl ? `
            <div class="mt-3 border-t border-border pt-3">
              <div class="flex items-center justify-between gap-2">
                <span class="inline-flex items-center gap-1.5 text-xs font-semibold text-foreground"><i data-lucide="key-round" class="h-3.5 w-3.5 text-muted-foreground"></i>SSH clone and push</span>
                <span class="fm-label">SSH</span>
              </div>
              <p class="mt-1 text-[11px] leading-4 text-muted-foreground">Register your public key in Settings. Your private key stays on your device; write permission is checked on every push.</p>
              <div class="mt-2 flex items-center gap-2">
                <input data-repo-ssh-url type="text" readonly value="${escapeHtml(sshUrl)}" aria-label="SSH clone and push URL" class="min-w-0 flex-1 rounded-md border border-border bg-secondary px-2 py-1 font-mono text-[11px] text-foreground outline-none focus:border-accent" />
                ${copyBtn(sshUrl, "Copy SSH URL")}
              </div>
              ${sshFullUrl !== sshUrl ? `<p class="mt-1 truncate text-[11px] leading-4 text-muted-foreground" title="${escapeHtml(sshFullUrl)}">Also: <span class="font-mono">${escapeHtml(sshFullUrl)}</span></p>` : ""}
              <div class="mt-2 flex items-center gap-2">
                <code class="min-w-0 flex-1 truncate rounded-md border border-border bg-secondary px-2 py-1 font-mono text-[11px] text-foreground">${escapeHtml(sshCloneCmd)}</code>
                ${copyBtn(sshCloneCmd, "Copy SSH clone command")}
              </div>
              <div class="mt-2 flex items-center gap-2">
                <code class="min-w-0 flex-1 truncate rounded-md border border-border bg-secondary px-2 py-1 font-mono text-[11px] text-foreground">${escapeHtml(sshPushCmd)}</code>
                ${copyBtn(sshPushCmd, "Copy SSH push-remote command")}
              </div>
            </div>` : ""}
        </div>
      </div>`;
  }

  const repoTreeContextMenuState = { path: "", kind: "tree" };

  function repoTreeContextMenu() {
    let menu = $("[data-repo-tree-context-menu]");
    if (menu) return menu;
    menu = document.createElement("div");
    menu.dataset.repoTreeContextMenu = "1";
    menu.className = "fm-popover fm-menu fixed z-50 hidden w-56";
    menu.setAttribute("role", "menu");
    document.body.appendChild(menu);
    return menu;
  }

  function closeRepoTreeContextMenu() {
    repoTreeContextMenu()?.classList.add("hidden");
  }

  function formatRepoContextCopyPath(repo, path) {
    const repoLabel = repoDisplayName(repo) || repoKey(repo) || "repo";
    return path ? `${repoLabel}/${path}` : repoLabel;
  }

  function formatRepoGitIgnoreLine(path, kind) {
    const clean = String(path || "").trim();
    if (!clean) return "";
    const normalized = clean.replace(/[\\/]?$/, "");
    return kind === "tree" ? `${normalized}/` : normalized;
  }

  function renderRepoTreeContextMenuItems(kind) {
    const downloadItem = kind === "blob"
      ? `<button type="button" data-repo-tree-context-action="download" role="menuitem" class="fm-menu-item"><i data-lucide="download" class="h-3.5 w-3.5 text-muted-foreground"></i><span>Download</span></button>`
      : "";
    return `
      <button type="button" data-repo-tree-context-action="open" role="menuitem" class="fm-menu-item"><i data-lucide="folder-open" class="h-3.5 w-3.5 text-muted-foreground"></i><span>Open</span></button>
      <button type="button" data-repo-tree-context-action="open-in-tab" role="menuitem" class="fm-menu-item"><i data-lucide="external-link" class="h-3.5 w-3.5 text-muted-foreground"></i><span>Open in new tab</span></button>
      <button type="button" data-repo-tree-context-action="copy-path" role="menuitem" class="fm-menu-item"><i data-lucide="copy" class="h-3.5 w-3.5 text-muted-foreground"></i><span>Copy Path</span></button>
      <button type="button" data-repo-tree-context-action="copy-relative-path" role="menuitem" class="fm-menu-item"><i data-lucide="link-2" class="h-3.5 w-3.5 text-muted-foreground"></i><span>Copy Relative Path</span></button>
      <button type="button" data-repo-tree-context-action="git-ignore" role="menuitem" class="fm-menu-item"><i data-lucide="circle-minus" class="h-3.5 w-3.5 text-muted-foreground"></i><span>Copy as .gitignore entry</span></button>
      ${downloadItem}
    `;
  }

  function openRepoTreeContextMenu(event, { path = "", kind = "tree" }) {
    const menu = repoTreeContextMenu();
    if (!state.selectedRepo) return;
    repoTreeContextMenuState.path = String(path || "");
    repoTreeContextMenuState.kind = kind;
    menu.dataset.repoTreeContextPath = repoTreeContextMenuState.path;
    menu.dataset.repoTreeContextKind = repoTreeContextMenuState.kind;
    menu.innerHTML = `
      <div class="fm-menu-heading truncate font-mono">${escapeHtml(formatRepoGitIgnoreLine(path, kind) || ".")}</div>
      <div class="fm-menu-divider"></div>
      ${renderRepoTreeContextMenuItems(kind)}
    `;
    window.lucide?.createIcons();
    menu.classList.remove("hidden");
    menu.style.visibility = "hidden";
    menu.style.left = `${event.clientX + 6}px`;
    menu.style.top = `${event.clientY + 6}px`;
    const rect = menu.getBoundingClientRect();
    const left = Math.max(8, Math.min(event.clientX + 6, window.innerWidth - rect.width - 8));
    const top = Math.max(8, Math.min(event.clientY + 6, window.innerHeight - rect.height - 8));
    menu.style.left = `${left}px`;
    menu.style.top = `${top}px`;
    menu.style.visibility = "visible";
  }

  async function handleRepoTreeContextAction(actionButton) {
    if (!state.selectedRepo) {
      closeRepoTreeContextMenu();
      return;
    }
    const action = actionButton?.dataset?.repoTreeContextAction || "";
    const path = repoTreeContextMenuState.path || "";
    const kind = repoTreeContextMenuState.kind || "tree";
    const repo = state.selectedRepo;
    if (!path && kind === "blob") {
      closeRepoTreeContextMenu();
      return;
    }
    if (action === "open") {
      if (kind === "tree") loadRepositoryTree(repo, path);
      else loadRepositoryBlob(repo, path);
      closeRepoTreeContextMenu();
      return;
    }
    if (action === "open-in-tab") {
      window.open(repoLinkUrl(repo, kind, path), "_blank", "noopener,noreferrer");
      closeRepoTreeContextMenu();
      return;
    }
    if (action === "download" && kind === "blob") {
      downloadRepoRawBlob({ rawUrl: repoRawUrl(repo, path), path });
      closeRepoTreeContextMenu();
      return;
    }
    if (action === "copy-path") {
      await copyTextToClipboard(formatRepoContextCopyPath(repo, path));
      closeRepoTreeContextMenu();
      return;
    }
    if (action === "copy-relative-path") {
      await copyTextToClipboard(path);
      closeRepoTreeContextMenu();
      return;
    }
    if (action === "git-ignore") {
      await copyTextToClipboard(formatRepoGitIgnoreLine(path, kind));
      closeRepoTreeContextMenu();
      return;
    }
    closeRepoTreeContextMenu();
  }

  function findRepository(key) {
    const wanted = String(key || "").trim();
    if (!wanted) return null;
    for (const group of groupRepositories(state.repositories)) {
      const origin = sourceOfTruth(group);
      if (repoMatchesKey(origin, wanted)) return origin;
      if ((group.members || []).some((member) => repoMatchesKey(member, wanted))) {
        return origin;
      }
    }
    return null;
  }

  async function findOrganizationRepository(key) {
    const wanted = String(key || "").trim();
    const parts = wanted.split("/");
    if (parts.length !== 2) return null;
    const organization = normalizeRepoSegment(parts[0]);
    const repository = normalizeRepoSegment(parts[1]);
    if (!organization || !repository) return null;

    let data;
    try {
      data = await fetchJson(
        `/api/orgs/${encodeURIComponent(organization)}/repos`,
      );
    } catch (_) {
      return null;
    }
    const linked = (Array.isArray(data?.repos) ? data.repos : []).find(
      (item) =>
        String(item?.repo || "").trim().toLowerCase() === repository.toLowerCase() &&
        normalizeRepoSegment(item?.node),
    );
    if (!linked) return null;

    const linkedOwner = normalizeRepoSegment(linked.node);
    const linkedRepository = normalizeRepoSegment(linked.repo);
    const linkedKey = `${linkedOwner}/${linkedRepository}`.toLowerCase();
    for (const group of groupRepositories(state.repositories)) {
      const member = (group.members || []).find(
        (item) => repoKey(item).toLowerCase() === linkedKey,
      );
      if (!member) continue;
      const origin = sourceOfTruth(group);
      const aliases = new Set(
        (Array.isArray(origin?._repoAliases) ? origin._repoAliases : [])
          .map((alias) => String(alias || "").toLowerCase()),
      );
      aliases.add(wanted.toLowerCase());
      aliases.add(linkedKey);
      return {
        ...origin,
        owner: organization,
        name: repository,
        // The catalog row names the account behind the publishing node, so
        // carrying it over would title this page "jett/forkmesh" on a visit to
        // /forkmesh/forkmesh. The URL resolved to an organization: that is the
        // repository's identity here.
        ownerKind: "organization",
        logicalOwner: organization,
        logicalOwners: [
          { kind: "organization", owner: organization },
          ...repoLogicalOwners(origin).filter((value) => value.kind !== "organization"),
        ],
        sshUrl: String(linked.sshUrl || origin.sshUrl || "").trim(),
        canonicalOwner: organization,
        canonicalName: repository,
        servingOwner: linkedOwner,
        servingName: linkedRepository,
        _repoAliases: [...aliases],
      };
    }
    return null;
  }

  // Opening a repo from any list/search control is a real page navigation now
  // (repo pages are their own documents). Prefer the organization address the
  // list already displays when the catalog resolved the key (alias groups),
  // falling back to the raw owner/name path — the repo page resolves it again
  // on boot.
  function openRepoPage(key) {
    const wanted = String(key || "").trim();
    if (!isRepoKey(wanted)) return;
    const repo = findRepository(wanted);
    const url = repo ? repoLinkUrl(repo) : "/" + wanted.split("/").map(encodeURIComponent).join("/");
    closeMobileDrawers();
    location.assign(url);
  }

  // A node's dot is only filled green when it is online *right now*; historical
  // uptime-leaderboard entries that have since gone offline render hollow so an
  // idle node no longer looks active (the right-rail bug in adhoc #86).
  function nodeDotClass(row, size) {
    return row.online
      ? `${size} fill-primary text-primary shrink-0`
      : `${size} fill-transparent text-muted-foreground/50 shrink-0`;
  }

  function nodeMetaLabel(row) {
    if (row.minutes) return formatCount(row.minutes);
    return row.online ? "live" : "";
  }

  // Node detail chips shown under each row in the full "Connected nodes" list
  // (not the compact home-page rail). Text fields fall back to an em dash when
  // a node hasn't reported them; counts default to 0 rather than a dash since
  // "no repos yet" is a real, distinct state from "field not tracked". CPU/RAM/
  // Disk aren't collected by any node -> worker path today (that telemetry is
  // desktop-only peer-room presence, see ServerNode::sampleSystemStats), so
  // they always render as a dash here for parity with the desktop Mirror nodes
  // panel's columns rather than being omitted.
  function nodeDetailChips(row) {
    const textChips = [
      ["Commit", row.commit ? String(row.commit).slice(0, 7) : ""],
      ["Synced", row.lastSync || ""],
      ["Platform", row.platform || ""],
      ["Version", row.version || ""],
      ["Node id", row.nodeId ? String(row.nodeId).slice(0, 12) : ""],
      ["CPU", ""],
      ["RAM", ""],
      ["Disk", ""],
    ];
    const countChips = [
      ["Size", formatSize(row.sizeBytes)],
      ["Issues", formatCount(row.issueCount)],
      ["Commits", formatCount(row.commitCount)],
      ["Branches", formatCount(row.branchCount)],
      ["Pulls", formatCount(row.pullCount)],
      ["Discussions", formatCount(row.discussionCount)],
      ["Worktrees", formatCount(row.worktreeCount)],
      ["Clones", formatCount(row.clonesServed)],
      ["Website", formatCount(row.websiteServed)],
      ["Artifacts", formatCount(row.artifactCount)],
    ];
    return [...textChips, ...countChips]
      .map(([label, value]) => `
        <span class="inline-flex items-center gap-1 rounded-md border border-border px-1.5 py-0.5 text-[10px] font-mono">
          <span class="text-muted-foreground">${escapeHtml(label)}</span>
          <span class="text-foreground">${escapeHtml(value === "" || value === undefined || value === null ? "-" : value)}</span>
        </span>`)
      .join("");
  }

  // The "Online only" toggle in the Connected nodes header hides offline nodes
  // (the default, matching the desktop Mirror nodes panel). Turning it off
  // surfaces nodes that have gone offline but still published a mirror record,
  // rendered inactive rather than live. Defaults to on until the user flips it.
  function networkOnlineOnly() {
    return state.networkOnlineOnly !== false;
  }

  // Header-search page filter for the Connected nodes list (adhoc #37): match
  // the name plus the identifying fields the row's chips already show, so a
  // hit is always something visible on the row.
  function networkRowsMatchingPageFilter(rows) {
    const queries = pageFilterQueries("");
    if (!queries.length) return rows;
    return rows.filter((row) => {
      const haystack = [
        row.name, row.platform, row.version, row.nodeId, row.commit,
        row.online ? "online live" : "offline",
      ].join(" ").toLowerCase();
      return queries.every((query) => haystack.includes(query));
    });
  }

  function renderNetworkRows(allRows) {
    const list = $("[data-network-node-list]");
    const total = $("[data-network-hosts]");
    const count = $("[data-network-node-count]");
    const toggle = $("[data-network-online-only]");
    const onlineOnly = networkOnlineOnly();
    const rows = networkRowsMatchingPageFilter(allRows);
    const onlineCount = rows.filter((row) => row.online).length;
    const offlineCount = rows.length - onlineCount;
    const visible = onlineOnly ? rows.filter((row) => row.online) : rows;

    if (toggle) {
      toggle.setAttribute("aria-pressed", onlineOnly ? "true" : "false");
      toggle.classList.toggle("border-border", onlineOnly);
      toggle.classList.toggle("text-foreground", onlineOnly);
      toggle.classList.toggle("text-muted-foreground", !onlineOnly);
      const label = toggle.querySelector("[data-network-online-only-label]");
      if (label) label.textContent = onlineOnly ? "Online only" : "Showing offline";
    }
    // The headline "Nodes" tile is written here, from the same row set that
    // produces the online/offline breakdown, so the total is always exactly
    // online + offline. It used to read stats.hosts (live nodes only) while the
    // list below it also carries nodes that went offline within the uptime
    // window, which showed "5 Nodes" above "5 online · 5 offline" (#623).
    if (total) total.textContent = formatCount(rows.length);
    if (count) {
      count.textContent = offlineCount
        ? `${formatCount(onlineCount)} online · ${formatCount(offlineCount)} offline`
        : `${formatCount(onlineCount)} online`;
    }
    if (list) {
      list.innerHTML = visible.length
        ? visible.map((row) => `
          <div class="px-4 py-3 hover:bg-secondary/50 transition-colors">
            <div class="flex items-center gap-4">
              <div class="flex items-center gap-2 flex-1 min-w-0">
                <i data-lucide="circle" class="${nodeDotClass(row, "w-2 h-2")}"></i>
                <span class="text-sm ${row.online ? "text-foreground" : "text-muted-foreground"} font-medium truncate font-mono">${escapeHtml(row.name || "node")}</span>
                ${row.online ? "" : '<span class="shrink-0 rounded border border-border px-1.5 py-0.5 text-[10px] font-mono text-muted-foreground">offline</span>'}
              </div>
              <span class="text-xs text-muted-foreground w-20 text-right font-mono">${escapeHtml(nodeMetaLabel(row))}</span>
            </div>
            <div class="mt-2 flex flex-wrap gap-1.5 pl-4">${nodeDetailChips(row)}</div>
          </div>
        `).join("")
        : `<div class="px-4 py-3 text-sm text-muted-foreground">${emptyListHtml(onlineOnly ? "No nodes online right now." : "No nodes yet.")}</div>`;
    }
  }

  function toggleNetworkOnlineOnly() {
    state.networkOnlineOnly = !networkOnlineOnly();
    renderNetworkRows(Array.isArray(state.networkNodeRows) ? state.networkNodeRows : []);
    window.lucide?.createIcons();
  }

  // One relay serves this panel — the control plane that just answered (or
  // failed to answer) the overview request. The "Active relays" tile, the relay
  // uptime tile and the header health pill all render from that single flag, so
  // the page can never report "0 Active relays" beside a healthy, Active relay
  // (#623). The tile used to show the flagship room's chat client count, which
  // says nothing about how many relays are up.
  function renderRelayState(active) {
    const relays = $("[data-network-relays]");
    const uptime = $("[data-network-uptime]");
    const health = $("[data-network-relay-health]");
    if (relays) relays.textContent = formatCount(active ? 1 : 0);
    if (uptime) uptime.textContent = active ? "Active" : "Idle";
    if (health) {
      health.classList.toggle("text-foreground", active);
      health.classList.toggle("text-muted-foreground", !active);
      // Re-baked rather than class-toggled: lucide replaces the <i> placeholder
      // with an <svg>, and renderNetwork's createIcons() pass upgrades this one.
      health.innerHTML = active
        ? '<i data-lucide="circle" class="w-1.5 h-1.5 fill-primary animate-pulse"></i>relay healthy'
        : '<i data-lucide="circle" class="w-1.5 h-1.5 fill-transparent text-muted-foreground/50"></i>relay unreachable';
    }
  }

  async function renderNetwork() {
    try {
      const overview = await fetchJson("/api/network/overview");
      const stats = overview.stats || {};
      const leaderboards = overview.leaderboards || {};
      const repos = Number(stats.repos) || 0;
      const uptime = Array.isArray(leaderboards.uptime)
        ? leaderboards.uptime
        : [];
      const nodeDetails = new Map(
        (Array.isArray(leaderboards.nodes) ? leaderboards.nodes : [])
          .map((node) => [String(node.name || "").trim().toLowerCase(), node]),
      );

      // The relay answered with usable data, so it is up: one active relay.
      renderRelayState(true);
      $("[data-network-repos]") && ($("[data-network-repos]").textContent = formatCount(repos));

      // Merge the 48h uptime leaderboard (name + minutes) with the set of nodes
      // that are online right now. Online nodes sort first and always appear even
      // with no accrued minutes yet, so a freshly-started node (e.g. a VM host)
      // shows up on the rail; offline leaderboard nodes stay listed but render
      // as inactive rather than looking live.
      const onlineNames = Array.isArray(stats.onlineNodes) ? stats.onlineNodes : [];
      const onlineSet = new Set(
        onlineNames.map((name) => String(name || "").trim().toLowerCase()).filter(Boolean),
      );
      const nodeRows = new Map();
      uptime.forEach((row) => {
        const name = String(row.name || "").trim();
        if (!name) return;
        const key = name.toLowerCase();
        nodeRows.set(key, {
          ...nodeDetails.get(key),
          name,
          minutes: Number(row.minutes) || 0,
          online: onlineSet.has(key),
        });
      });
      onlineNames.forEach((name) => {
        const clean = String(name || "").trim();
        if (!clean) return;
        const key = clean.toLowerCase();
        if (!nodeRows.has(key)) {
          nodeRows.set(key, { ...nodeDetails.get(key), name: clean, minutes: 0, online: true });
        }
      });
      const rows = [...nodeRows.values()].sort(
        (a, b) =>
          Number(b.online) - Number(a.online) ||
          b.minutes - a.minutes ||
          a.name.localeCompare(b.name),
      );
      state.networkNodeRows = rows;
      renderNetworkRows(rows);
    } catch (_) {
      // Nothing came back from the relay, so the pill must stop claiming health
      // while the list underneath says the data is unavailable.
      renderRelayState(false);
      $("[data-network-node-list]") && ($("[data-network-node-list]").innerHTML =
        '<div class="px-4 py-3 text-sm text-muted-foreground">Network data is unavailable right now.</div>');
    } finally {
      window.lucide?.createIcons();
    }
  }

  function notificationIcon(kind) {
    const icons = {
      mention: "at-sign",
      subscribed: "bell-ring",
      issue_opened: "circle-dot",
      pull_submitted: "git-pull-request",
      repo_activity: "git-commit-horizontal",
      code_push: "git-commit-horizontal",
      issue_assigned: "circle-dot",
      repo_shared: "share-2",
      security_advisory: "shield-alert",
      bounty_funded: "badge-dollar-sign",
      bounty_paid: "circle-dollar-sign",
      release_published: "tag",
      host_online: "wifi",
      host_offline: "wifi-off",
      credits_refilled: "circle-dollar-sign",
      pending_inbox: "inbox",
      mirror_request: "radio",
      pending_reward: "circle-dollar-sign",
      org_succession: "building-2",
      repository_hosted: "cloud",
      account_email_sent: "mail-check",
      organization_task_started: "clipboard-list",
      organization_task_activity: "clipboard-list",
      organization_task_update: "clipboard-list",
      calendar_invitation: "calendar-plus",
      calendar_reminder: "alarm-clock",
      calendar_rsvp: "calendar-check",
      chat_webhook: "message-square",
      channel_unread: "message-square",
      direct_unread: "message-square",
      operational_alert: "triangle-alert",
      security_alert: "shield-alert",
      device_added: "smartphone",
      tip_sent: "circle-dollar-sign",
      error_group: "triangle-alert",
      plugin_activity: "puzzle",
      timer_finished: "timer",
      desktop_ping: "monitor",
      hackathon_squad: "users",
      hackathon_reward: "gift",
      badge: "award",
      package_review: "star",
    };
    return icons[String(kind || "").trim().toLowerCase()] || "bell";
  }

  function notificationTimeLabel(ts) {
    const value = Number(ts) || 0;
    if (!value) return "just now";
    const elapsed = Math.max(0, Date.now() - value);
    const minutes = Math.floor(elapsed / 60000);
    if (minutes < 1) return "just now";
    if (minutes < 60) return `${minutes}m ago`;
    const hours = Math.floor(minutes / 60);
    if (hours < 24) return `${hours}h ago`;
    return formatDate(value);
  }

  function renderNotificationPreview() {
    const countEl = $("[data-notification-count]");
    const badge = $("[data-notification-badge]");
    const list = $("[data-notification-preview-list]");
    const unread = Number(state.notificationUnread) || 0;
    if (countEl) countEl.textContent = unread === 1 ? "1 new" : `${unread} new`;
    badge?.classList.toggle("hidden", unread <= 0);
    if (!list) return;
    const items = state.notifications.slice(0, 5);
    if (!items.length) {
      list.innerHTML = '<div class="px-3 py-4 text-xs text-muted-foreground">No pings yet. Mentions, PRs, assignments, shares, bounties, releases, and host status changes will appear here.</div>';
      return;
    }
    list.innerHTML = items.map((item) => `
      <button type="button" data-notification-open="${escapeHtml(item.id || "")}" class="w-full px-3 py-2 text-left hover:bg-secondary transition-colors ${item.readAt ? "opacity-70" : ""}">
        <div class="flex items-start gap-2">
          <i data-notification-kind-icon data-lucide="${notificationIcon(item.kind)}" class="mt-0.5 h-3.5 w-3.5 shrink-0 text-muted-foreground"></i>
          <div class="min-w-0 flex-1">
            <p class="truncate text-xs font-medium text-foreground">${escapeHtml(repoPublicText(item.title) || "Ping")}</p>
            <p class="mt-0.5 truncate text-[11px] text-muted-foreground">${escapeHtml(repoPublicText(item.body) || repoPublicKey(item.repo) || `${platformName()} update`)}</p>
          </div>
          <span class="shrink-0 text-[10px] text-muted-foreground font-mono">${escapeHtml(notificationTimeLabel(item.ts))}</span>
        </div>
      </button>
    `).join("");
    window.lucide?.createIcons();
  }

  function renderNotificationDetail(item) {
    const detail = $("[data-notification-modal-detail]");
    if (!detail) return;
    if (!item) {
      detail.innerHTML = '<div class="text-sm text-muted-foreground">Select a ping to read it.</div>';
      return;
    }
    detail.innerHTML = `
      <div class="flex items-start gap-3">
        <span class="flex h-9 w-9 shrink-0 items-center justify-center rounded-full border border-border bg-secondary text-muted-foreground">
          <i data-notification-kind-icon data-lucide="${notificationIcon(item.kind)}" class="h-4 w-4"></i>
        </span>
        <div class="min-w-0 flex-1">
          <p class="text-sm font-semibold text-foreground">${escapeHtml(repoPublicText(item.title) || "Ping")}</p>
          <p class="mt-1 text-xs text-muted-foreground">${escapeHtml(notificationTimeLabel(item.ts))}${item.repo ? ` · ${escapeHtml(repoPublicKey(item.repo))}` : ""}</p>
        </div>
      </div>
      <p class="mt-5 whitespace-pre-wrap text-sm leading-6 text-muted-foreground">${escapeHtml(repoPublicText(item.body) || `${platformName()} notification`)}</p>
      ${organizationTaskDetailsHtml(item)}
      ${mirrorRequestActionsHtml(item)}
      <div class="mt-5 flex flex-wrap items-center gap-2">
        ${item.href ? `<a href="${escapeHtml(repoPublicHref(item.href))}" class="inline-flex h-9 items-center justify-center rounded-md border border-border px-3 text-xs font-medium text-foreground hover:bg-secondary transition-colors">Open context</a>` : ""}
        ${pingTurnOffLinkHtml(item)}
      </div>
    `;
    window.lucide?.createIcons();
  }

  // Where a ping's settings live: its own row on the Pings settings page when
  // the kind is in the catalog, else the top of that page.
  function pingSettingsHref(kind) {
    const key = String(kind || "");
    return pingKindLabel(key)
      ? `/dashboard/settings/notifications#ping-kind-${encodeURIComponent(key)}`
      : "/dashboard/settings/notifications";
  }

  function pingTurnOffLinkHtml(item) {
    const label = pingKindLabel(item?.kind);
    if (!label) return "";
    return `<a href="${escapeHtml(pingSettingsHref(item.kind))}" data-ping-settings-link="${escapeHtml(item.kind)}" class="inline-flex h-9 items-center justify-center gap-1.5 rounded-md px-3 text-xs font-medium text-muted-foreground hover:bg-secondary hover:text-foreground transition-colors"><i data-lucide="bell-off" class="h-3.5 w-3.5"></i>Turn off “${escapeHtml(label)}” pings</a>`;
  }

  // Organization task pings (adhoc #147) carry the whole event on meta, so the
  // reader gets who/what/which org spelled out as fields instead of having to
  // parse the one-line summary the ping list shows.
  function organizationTaskDetailsHtml(item) {
    if (!item || !String(item.kind || "").startsWith("organization_task")) return "";
    const meta = item.meta && typeof item.meta === "object" ? item.meta : {};
    const assignee = String(meta.assignee || "");
    const assigneeLabel = meta.assigneeKind === "agent"
      ? "An agent"
      : (assignee ? `@${assignee}` : "Unassigned");
    const priority = Number(meta.priority) || 0;
    const status = ({
      idle: "Not started",
      active: "In progress",
      done: "Done",
    })[String(meta.status || "")] || String(meta.status || "");
    const rows = [
      ["Who", meta.actor ? `@${meta.actor}` : ""],
      ["What", `${String(meta.action || "updated")} a task`],
      ["Task", meta.taskTitle || ""],
      ["Organization", meta.organization || ""],
      ["Department", meta.department || ""],
      ["Team", meta.team || ""],
      ["Assigned to", assigneeLabel],
      ["Priority", priority > 0 ? String(priority) : ""],
      ["Status", status],
      ["Repository", meta.repository || ""],
    ].filter(([, value]) => String(value || "").trim());
    if (!rows.length) return "";
    return `
      <dl class="mt-5 grid grid-cols-[auto,1fr] gap-x-4 gap-y-1 rounded-md border border-border bg-secondary/40 px-3 py-2 text-xs">
        ${rows.map(([label, value]) => `
          <dt class="text-muted-foreground">${escapeHtml(label)}</dt>
          <dd class="min-w-0 break-words text-foreground">${escapeHtml(String(value))}</dd>
        `).join("")}
      </dl>
    `;
  }

  // Accept/Reject controls on an incoming "someone asked your node to mirror
  // their repo" notification (issue #385). Only the still-pending request the
  // recipient can act on gets buttons; replies ("X accepted…") carry none.
  function mirrorRequestActionsHtml(item) {
    if (!item || item.kind !== "mirror_request") return "";
    const meta = item.meta && typeof item.meta === "object" ? item.meta : {};
    if (String(meta.state || "") !== "pending") return "";
    const id = String(meta.requestId || "");
    if (!id) return "";
    const enc = escapeHtml(id);
    return `
      <div data-mirror-request-actions="${enc}" class="mt-5 flex items-center gap-2">
        <button type="button" data-mirror-request-accept="${enc}" class="inline-flex h-9 items-center justify-center rounded-md bg-cta px-3 text-xs font-medium text-cta-foreground hover:bg-cta/90 transition-colors">Accept &amp; mirror</button>
        <button type="button" data-mirror-request-reject="${enc}" class="inline-flex h-9 items-center justify-center rounded-md border border-border px-3 text-xs font-medium text-foreground hover:bg-secondary transition-colors">Decline</button>
        <span data-mirror-request-hint class="text-[11px] text-muted-foreground"></span>
      </div>
    `;
  }

  async function resolveMirrorRequest(id, action, trigger) {
    const node = state.session?.nodeName || "";
    if (!node || !id) return;
    const container = trigger?.closest("[data-mirror-request-actions]");
    const hint = container?.querySelector("[data-mirror-request-hint]");
    container?.querySelectorAll("button").forEach((b) => { b.disabled = true; });
    if (hint) hint.textContent = action === "accept" ? "Accepting…" : "Declining…";
    try {
      const res = await fetch("/api/mirror-requests", {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({
          node, action, requestId: id,
          sessionToken: state.session?.sessionToken || "",
        }),
      });
      if (!res.ok) throw new Error("request failed");
      if (hint) hint.textContent = action === "accept"
        ? "Accepted — your node will start mirroring it shortly."
        : "Declined.";
      if (container) container.querySelectorAll("button").forEach((b) => b.remove());
      await loadNotifications();
    } catch (_) {
      if (hint) hint.textContent = "Could not update the request. Try again.";
      container?.querySelectorAll("button").forEach((b) => { b.disabled = false; });
    }
  }

  // "Ask a node to mirror your repo" (issue #385): the owner types a node name;
  // that node's holder gets a notification and, if they accept, their node
  // starts mirroring this repo. Only shown to the repo owner (see
  // renderMirrorRequestForm) — the worker re-checks ownership from the session.
  async function askNodeToMirror(trigger) {
    const repo = state.selectedRepo;
    if (!repo || !isRepoOwner(repo)) return;
    const wrap = trigger.closest("[data-mirror-request]") || document;
    const input = wrap.querySelector("[data-mirror-request-target]");
    const hint = wrap.querySelector("[data-mirror-request-hint]");
    const setHint = (text, tone) => {
      if (!hint) return;
      hint.className = `mt-1.5 text-[11px] ${tone === "bad" ? "text-destructive" : tone === "good" ? "text-muted-foreground" : "text-muted-foreground"}`;
      hint.textContent = text;
    };
    const target = String(input?.value || "").trim().toLowerCase();
    if (!target) {
      setHint("Enter the node name to ask.", "bad");
      input?.focus();
      return;
    }
    if (target === String(repo.owner || "").trim().toLowerCase()) {
      setHint("That's this repo's own node.", "bad");
      return;
    }
    trigger.disabled = true;
    setHint("Sending request…");
    try {
      const res = await fetch("/api/mirror-requests", {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({
          node: state.session?.nodeName || "",
          action: "create",
          target,
          owner: String(repo.owner || ""),
          repo: String(repo.name || ""),
          sessionToken: state.session?.sessionToken || "",
        }),
      });
      const body = await res.json().catch(() => ({}));
      if (!res.ok || !body.ok) {
        const reason = {
          target_not_found: "No node by that name.",
          repo_not_found: "This repo isn't published yet.",
          private_repo: "Only public repos can be mirrored this way.",
          forbidden: "You can only ask others to mirror your own repos.",
          self_target: "That's this repo's own node.",
        }[String(body.error || "")] || "Could not send the request.";
        setHint(reason, "bad");
        trigger.disabled = false;
        return;
      }
      if (input) input.value = "";
      setHint(`Asked ${target} to mirror this repo.`, "good");
    } catch (_) {
      setHint("Could not send the request. Try again.", "bad");
    } finally {
      trigger.disabled = false;
    }
  }

  function renderMirrorRequestForm(repo) {
    const form = $("[data-mirror-request]");
    if (!form) return;
    form.hidden = !isRepoOwner(repo);
  }

  function renderNotificationModal() {
    const list = $("[data-notification-modal-list]");
    if (!list) return;
    if (!state.notifications.length) {
      list.innerHTML = '<div class="p-3 text-xs text-muted-foreground">No pings yet.</div>';
      renderNotificationDetail(null);
      return;
    }
    const selected = state.notifications.find((item) => item.id === state.selectedNotificationId)
      || state.notifications[0];
    state.selectedNotificationId = selected?.id || "";
    list.innerHTML = state.notifications.map((item) => {
      const active = item.id === state.selectedNotificationId;
      const settingsTitle = pingKindLabel(item.kind)
        ? `${pingKindLabel(item.kind)}: ping settings`
        : "Ping settings";
      // The trailing gear is the "settings" column: it opens this kind's row
      // on the Pings settings page, where it can be switched off.
      return `
        <div data-notification-row class="mb-1 flex items-stretch gap-1 rounded-md ${active ? "bg-secondary text-foreground" : "text-muted-foreground hover:bg-secondary/60 hover:text-foreground"}">
          <button type="button" data-notification-open="${escapeHtml(item.id || "")}" class="min-w-0 flex-1 rounded-md px-3 py-2 text-left transition-colors">
            <span class="flex min-w-0 items-start gap-2">
              <span class="mt-0.5 flex h-6 w-6 shrink-0 items-center justify-center rounded-full border border-border bg-background text-muted-foreground">
                <i data-notification-kind-icon data-lucide="${notificationIcon(item.kind)}" class="h-3.5 w-3.5"></i>
              </span>
              <span class="min-w-0 flex-1">
                <span class="block truncate text-xs font-medium">${escapeHtml(item.title || "Ping")}</span>
                <span class="mt-1 block truncate text-[11px] font-mono">${escapeHtml(item.repo || item.kind || "forkmesh")}</span>
              </span>
            </span>
          </button>
          <a href="${escapeHtml(pingSettingsHref(item.kind))}" data-ping-settings-link="${escapeHtml(pingKindLabel(item.kind) ? item.kind : "")}" title="${escapeHtml(settingsTitle)}" aria-label="${escapeHtml(settingsTitle)}" class="flex w-8 shrink-0 items-center justify-center rounded-md text-muted-foreground hover:bg-background/60 hover:text-foreground transition-colors">
            <i data-lucide="settings" class="h-3.5 w-3.5"></i>
          </a>
        </div>
      `;
    }).join("");
    renderNotificationDetail(selected);
  }

  async function loadNotifications() {
    const node = state.session?.nodeName || "";
    if (!node) {
      state.notifications = [];
      state.notificationUnread = 0;
      renderNotificationPreview();
      renderHomeFeed();
      return;
    }
    try {
      const data = await fetchJson(`/api/notifications?node=${encodeURIComponent(node)}`);
      state.notifications = Array.isArray(data.notifications) ? data.notifications : [];
      state.notificationUnread = Number(data.unread) || 0;
      // The relay owns the pause deadline, so this refresh is how a pause set
      // on another surface (the desktop app) reaches an open dashboard.
      state.session = {
        ...(state.session || {}),
        notificationPauseUntil: Number(data.pauseUntil) || 0,
        notificationQuietUntil: Number(data.quietUntil) || 0,
      };
    } catch (_) {
      state.notifications = [];
      state.notificationUnread = 0;
    }
    renderNotificationPreview();
    renderNotificationModal();
    renderPingPause();
    renderHomeFeed();
    celebrateNewPings(state.notifications);
  }

  function celebrateNewPings(notifications) {
    if (!notifications.some((item) => item?.meta?.celebrate && !item.readAt)) return;
    if (Number(state.session?.notificationPauseUntil) > Date.now()) return;
    import("/member-welcome.js")
      .then(({ celebrateFreshPings }) => celebrateFreshPings(notifications))
      .catch(() => {});
  }

  function setNotificationDropdownOpen(open) {
    const toggle = $("#notificationToggle");
    const dropdown = $("#notificationDropdown");
    dropdown?.classList.toggle("hidden", !open);
    toggle?.setAttribute("aria-expanded", open ? "true" : "false");
    // The pause presets are a submenu of this dropdown, so they collapse with
    // it rather than being left open behind the next thing that opens.
    if (!open) setPingPauseMenuOpen(false);
  }

  function setAccountMenuOpen(open) {
    const toggle = $("#accountMenuToggle");
    const dropdown = $("#accountMenuDropdown");
    dropdown?.classList.toggle("hidden", !open);
    toggle?.setAttribute("aria-expanded", open ? "true" : "false");
  }

  function setNotificationModalOpen(open) {
    const modal = $("#notificationModal");
    if (!modal) return;
    modal.classList.toggle("hidden", !open);
    modal.classList.toggle("flex", open);
    if (open) renderNotificationModal();
  }

  async function markNotificationsRead(ids = [], all = false) {
    const node = state.session?.nodeName || "";
    if (!node) return;
    try {
      await fetch("/api/notifications", {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({
          node, ids, all,
          sessionToken: state.session?.sessionToken || "",
        }),
      });
    } catch (_) {}
    await loadNotifications();
  }

  async function openNotification(id, modal = false) {
    const item = state.notifications.find((candidate) => candidate.id === id);
    if (!item) return;
    state.selectedNotificationId = id;
    renderNotificationModal();
    if (!item.readAt) await markNotificationsRead([id]);
    if (modal) {
      setNotificationDropdownOpen(false);
      setNotificationModalOpen(true);
    }
  }

  // Each page is its own document (marked <body data-page="...">). Boot runs
  // the shared chrome first, then that page's init — the page's own markup is
  // already visible at parse time, so there is no flash-then-swap.
  function currentPage() {
    return document.body?.dataset?.page || "home";
  }

  // Clean path per legacy ?section= name — links from old builds and the
  // desktop app still arrive as /dashboard?section=X. The Worker 308s these
  // too; this client shim is belt-and-braces for cached home documents.
  const SECTION_PATHS = {
    home: "/dashboard",
    repos: "/dashboard/repos",
    network: "/dashboard/network",
    chat: "/dashboard/chat",
    profile: "/dashboard/settings",
    "profile-overview": "/dashboard/profile",
    "profile-repositories": "/dashboard/profile/repositories",
  };

  function legacyRedirectTarget() {
    if (location.pathname !== "/dashboard") return "";
    const params = new URLSearchParams(location.search);
    const section = (params.get("section") || "").trim();
    if (section && SECTION_PATHS[section]) {
      params.delete("section");
      const rest = params.toString();
      return SECTION_PATHS[section] + (rest ? `?${rest}` : "");
    }
    const repo = (params.get("repo") || "").trim();
    if (repo.includes("/")) {
      return "/" + repo.split("/").map(encodeURIComponent).join("/");
    }
    return "";
  }

  // Fetches the repository catalog once and fans it out to whatever containers
  // exist on this page (sidebar list is chrome on every page; the repos list,
  // home feed, and profile views fill in when present). Never awaited before
  // first paint — each page shows its own skeleton immediately. The repo page
  // awaits this promise before resolving its /owner/repo path.
  let repositoriesReady = null;

  // Resolves once the canonical account snapshot is available. Current login
  // sessions already carry it; legacy/cross-origin sessions take one fallback
  // lookup. The owner-only Agents tab (adhoc #182) is gated on nodes/isAdmin,
  // so the repo page waits before deciding whether an /owner/repo/agents deep
  // link is a real tab route or must collapse to Code (adhoc #93).
  let canonicalProfileReady = null;

  // The catalog is public, but it is the address book needed before a repo
  // page can send reads straight to a mirror. Keep the last validated response
  // per browser so repeat navigation can resolve and preconnect immediately;
  // a background refresh replaces the UI as soon as App answers.
  const REPOSITORY_CATALOG_STORAGE_KEY = "forkmesh.repositoryCatalog.v1";
  const REPOSITORY_CATALOG_STORAGE_MAX_MS = 5 * 60 * 1000;

  function readStoredRepositoryCatalog() {
    try {
      const stored = JSON.parse(
        localStorage.getItem(REPOSITORY_CATALOG_STORAGE_KEY) || "null",
      );
      if (
        !stored
        || !Array.isArray(stored.repositories)
        || Date.now() - Number(stored.savedAt || 0)
          > REPOSITORY_CATALOG_STORAGE_MAX_MS
      ) return null;
      return stored.repositories;
    } catch (_) {
      return null;
    }
  }

  function storeRepositoryCatalog(repositories) {
    if (!Array.isArray(repositories)) return;
    try {
      localStorage.setItem(REPOSITORY_CATALOG_STORAGE_KEY, JSON.stringify({
        savedAt: Date.now(),
        repositories: withoutDeletedRepositories(repositories),
      }));
    } catch (_) {}
  }

  const REPOSITORY_DELETED_STORAGE_KEY = "forkmesh.repositoryDeleted.v1";
  const REPOSITORY_DELETED_STORAGE_MAX_MS = 10 * 60 * 1000;

  function readDeletedRepositoryKeys() {
    try {
      const stored = JSON.parse(
        localStorage.getItem(REPOSITORY_DELETED_STORAGE_KEY) || "{}",
      );
      const now = Date.now();
      const live = {};
      for (const [key, at] of Object.entries(stored || {})) {
        if (now - Number(at || 0) <= REPOSITORY_DELETED_STORAGE_MAX_MS) live[key] = at;
      }
      return live;
    } catch (_) {
      return {};
    }
  }

  function rememberDeletedRepository(repo) {
    const deleted = readDeletedRepositoryKeys();
    deleted[repoKey(repo).toLowerCase()] = Date.now();
    try {
      localStorage.setItem(REPOSITORY_DELETED_STORAGE_KEY, JSON.stringify(deleted));
    } catch (_) {}
    const stored = readStoredRepositoryCatalog();
    if (stored) storeRepositoryCatalog(stored);
  }

  function forgetDeletedRepository(repo) {
    const deleted = readDeletedRepositoryKeys();
    const key = repoKey(repo || {}).toLowerCase();
    if (!deleted[key]) return;
    delete deleted[key];
    try {
      localStorage.setItem(REPOSITORY_DELETED_STORAGE_KEY, JSON.stringify(deleted));
    } catch (_) {}
  }

  function withoutDeletedRepositories(repositories) {
    const deleted = readDeletedRepositoryKeys();
    if (!Object.keys(deleted).length) return repositories;
    return repositories.filter(
      (repo) => !deleted[repoKey(repo || {}).toLowerCase()],
    );
  }

  async function refreshRepositoryCatalog({ fresh = false } = {}) {
    const startedAt = interfaceNow();
    const data = await fetchJson("/api/repositories", { fresh });
    storeRepositoryCatalog(data.repositories);
    renderRepositories(data.repositories, state.session);
    recordInterfaceMetric("browse", startedAt, "remote");
  }

  // Boot/shared-chrome loads take the cached path (browser + edge cache honor
  // the server's max-age); only explicit user refresh actions pass fresh:true.
  async function loadRepositories({ fresh = false } = {}) {
    if (dashboardMockRepositoriesEnabled()) {
      const startedAt = interfaceNow();
      renderRepositories(dashboardMockRepositories(), state.session);
      recordInterfaceMetric("browse", startedAt, "local");
      return;
    }
    const stored = fresh ? null : readStoredRepositoryCatalog();
    if (stored) {
      const startedAt = interfaceNow();
      renderRepositories(stored, state.session);
      recordInterfaceMetric("browse", startedAt, "local");
      // Do not gate the repo page on the network: it can already resolve its
      // route and start direct mirror reads. The public snapshot refreshes the
      // catalog behind it and updates every list when it arrives.
      void refreshRepositoryCatalog().catch(() => {});
      return;
    }
    try {
      await refreshRepositoryCatalog({ fresh });
    } catch (_) {
      state.repositoriesLoading = false;
      const list = $("#repoList");
      const count = $("[data-repo-count]");
      if (count) count.textContent = "Unavailable";
      if (list) {
        list.innerHTML = `<div role="status" class="fm-blankslate">
          <i data-lucide="cloud-off"></i>
          <span class="fm-blankslate-title">Repositories are unavailable.</span>
          <span class="fm-blankslate-desc">Check your connection or mirror, then try again.</span>
          <button data-repositories-retry type="button" class="fm-btn fm-btn-sm mt-3">Retry repository list</button>
        </div>`;
        window.lucide?.createIcons();
      }
      renderSidebarRepositories(state.session);
      renderHomeFeed();
    }
  }

  // Returns false when boot is aborting into a redirect (login bounce, legacy
  // URL shim) so the page init doesn't race the navigation.
  function initSharedChrome() {
    const session = readSession();
    state.session = session;

    const grant = pendingLinkGrant();
    if (grant && !session?.nodeName) {
      // A link grant arrived but nobody is logged in: bounce through login and
      // come straight back with the grant intact so the link completes then.
      location.replace("/login?next=" + encodeURIComponent(`${location.pathname}${location.search}`));
      return false;
    }
    if (grant && currentPage() !== "settings") {
      // The desktop app hard-codes /dashboard?link_node=... — the Nodes panel
      // lives on the settings document now. Carry the grant params over
      // untouched; offerLinkGrant strips them there.
      location.replace("/dashboard/settings" + location.search);
      return false;
    }
    const legacyTarget = legacyRedirectTarget();
    if (legacyTarget) {
      location.replace(legacyTarget);
      return false;
    }

    // Guests can browse repositories without an account: instead of bouncing
    // signed-out visitors back to the landing page, the header swaps the
    // profile/notification controls for a Sign Up / Log In link.
    const guest = !session || (!session.nodeName && !session.email);
    document.documentElement?.toggleAttribute?.("data-guest", guest);
    if (guest) {
      // The profile and settings documents are account pages — signed-out
      // visitors have no profile to show there, so bounce through login and
      // come back once they have one. /@name public profiles share the profile
      // document and stay open to guests.
      const page = currentPage();
      const accountPage = page === "settings" ||
        ((page === "profile" || page === "profile-repositories") && !publicProfileNameFromPath());
      if (accountPage) {
        location.replace("/login?next=" + encodeURIComponent(`${location.pathname}${location.search}`));
        return false;
      }
      const authLink = $("[data-guest-auth-link]");
      if (authLink) {
        authLink.classList.remove("hidden");
        authLink.classList.add("inline-flex");
      }
      $("[data-profile-account-controls]")?.classList.add("hidden");
      $("#notificationToggle")?.classList.add("hidden");
      $("#agentModalToggle")?.classList.add("hidden");
      // Keep the presence cookie honest: localStorage says logged out, so the
      // Worker must stop 302ing / to the dashboard.
      document.cookie = "forkmesh_session=; Path=/; Max-Age=0; SameSite=Lax";
    }

    if (guest) {
      // No fabricated "guest" identity: the chrome keeps its baked defaults;
      // only the per-page header context still needs to be set.
      renderHeaderContext();
    } else {
      renderProfile(session);
    }
    // Unverified accounts never reach the product (issue #592). Current login
    // sessions already carry the authoritative flag; the canonical-profile
    // helper performs one fallback read for a session that predates it.
    if (bounceToVerification(session)) return false;
    if (session?.nodeName) {
      if (grant) offerLinkGrant(grant);
      canonicalProfileReady = hydrateCanonicalProfile(session)
        .then((nextSession) => {
          if (bounceToVerification(nextSession)) return;
          return loadNotifications();
        })
        .catch(() => {});
    }
    repositoriesReady = loadRepositories();
    return true;
  }

  function initHomePage() {
    renderHomeBlogPosts();
    // The blog card fills in from the edge-cached feed; the baked markup
    // already shows its loading state.
    void loadHomeBlogPosts();
    void ensureHomeOrganizationRepositories();
    // Feed + top repositories fill in when loadRepositories()/loadNotifications()
    // resolve — both re-render the home containers.
    void loadHomeAgentSessions();
    window.addEventListener("forkmesh:cloud-agents", () => { void loadHomeAgentSessions(); });
  }

  function initReposPage() {
    // The catalog fetch from initSharedChrome() owns this page's content; the
    // baked markup already shows the loading state.
    loadExternalRepositories();
    // The home rail's "New" links here to start the create-and-mirror flow.
    consumeNewRepositoryRequest();
  }

  function initNewRepoPage() {
    prepareNewRepoForm();
  }

  function initNetworkPage() {
    renderNetwork();
  }

  function initChatPage() {
    // dashboard-chat.js self-boots off the #fullChatMessages markup.
  }

  function initProfileOverviewPage() {
    // /@name serves this same document in public-profile mode: render the
    // named account's public data instead of the logged-in session (viewing
    // your own /@name keeps the full owner view).
    const publicName = publicProfileNameFromPath();
    const own = String(state.session?.nodeName || "").toLowerCase();
    if (publicName && publicName !== own) {
      loadPublicProfile(publicName);
      return;
    }
    renderProfilePage(state.session);
    refreshPublicProfile(state.session);
  }

  function initSettingsPage() {
    renderProfilePage(state.session);
    refreshPublicProfile(state.session);
    initProfileAvatarEditor();
    const patreonOutcome = new URLSearchParams(location.search).get("patreon");
    // "Reconnect node" in the header opens the Nodes tab directly; honour the
    // marker even on a bare /dashboard/settings?from=reconnect URL so the click
    // never lands on public-profile with no explanation (issue #640).
    const section = patreonOutcome
      ? "account"
      : arrivedFromNodeReconnect()
        ? "nodes"
        : settingsSectionFromPath();
    setSettingsSection(section, { scroll: false });
  }

  async function initRepoPage() {
    const requested = requestedRepoKey();
    const detail = $("[data-repo-detail]");
    const headerContext = $("[data-dashboard-header-context]");
    if (headerContext && requested) headerContext.textContent = repoPublicKey(requested);
    // A node that reads the repository locally sends the page with its first
    // view already rendered (app/host/forkmesh_host/repo_ssr.py); keep that
    // on screen until the full interface replaces it.
    if (detail && requested && !detail.querySelector("[data-repo-ssr], [data-repo-frame]")) {
      detail.innerHTML = `<p class="text-sm text-muted-foreground">${loadingHtml("Loading repository…")}</p>`;
    }
    // findRepository needs the catalog (alias/canonical grouping), so this page
    // does wait on the shared fetch before rendering the detail body.
    const viewerPermissionReady = requested && state.session?.nodeName
      ? loadRepoViewerPermission(requested)
      : null;
    await (repositoriesReady || loadRepositories());
    let repo = requested ? findRepository(requested) : null;
    if (
      repo &&
      requested &&
      repoKey(repo).toLowerCase() !== requested.toLowerCase()
    ) {
      // A catalog alias normally resolves to its backing source record. Before
      // accepting that canonical identity, check whether the URL is a durable
      // organization alias so subsequent tab/tree navigation keeps the public
      // /org/repo address instead of appearing to redirect to /node/repo.
      repo = (await findOrganizationRepository(requested)) || repo;
    } else if (!repo && requested) {
      // Organization URLs are public aliases backed by a node-owned catalog
      // record. The catalog deliberately publishes only the signing node's
      // identity, so resolve the public org link on a direct-page visit and
      // keep the requested organization identity while every data request
      // continues through the Worker's existing org-alias authorization path.
      repo = await findOrganizationRepository(requested);
    }
    if (
      !repo &&
      requested &&
      (detail?.querySelector("[data-repo-ssr]") || state.session?.sessionToken)
    ) {
      try {
        await loadRepositories({ fresh: true });
      } catch (_) {}
      repo = findRepository(requested) || (await findOrganizationRepository(requested));
    }
    if (repo) {
      // Owner/admin Agents and owner-only Settings depend on nodes/isAdmin
      // fields that land after canonical profile hydration. On a hard refresh
      // of either private control route, wait before deciding whether the tab
      // exists so the URL is not incorrectly collapsed back to Code.
      const routeParts = repoRouteParts();
      if (
        (routeParts[2] === "agents" && !sessionCanAssignAgent(repo)) ||
        (routeParts[2] === "settings" && !sessionAdministersRepo(repo))
      ) {
        await (canonicalProfileReady || Promise.resolve());
      }
      await (viewerPermissionReady || Promise.resolve());
      if (state.session?.nodeName && !sessionAdministersRepo(repo)) {
        await loadRepoViewerPermission(repo);
      }
      renderRepoDetail(repo);
      return;
    }
    if (detail) renderRepoNotFound(detail, requested);
  }

  // A repository URL that resolves to nothing is the most-visited dead end on
  // this site: every stale link, every crawler walking a name we have never
  // held, and every visitor who mistyped an owner lands here. It used to be
  // one grey sentence in the corner of an otherwise empty page, which reads
  // like the app broke rather than like the address is wrong — so say what
  // happened and offer the ways out (adhoc #1642). The name is echoed back
  // escaped: it is whatever was in the URL bar.
  function renderRepoNotFound(detail, requested) {
    const name = escapeHtml(repoPublicKey(requested || ""));
    detail.innerHTML = `
      <section class="flex flex-col items-center gap-4 rounded-lg border border-border bg-card px-6 py-16 text-center">
        <p class="font-mono text-4xl font-semibold text-muted-foreground">404</p>
        <h2 class="text-lg font-semibold text-foreground">Repository not found</h2>
        <p class="max-w-md text-sm text-muted-foreground">
          ${name
            ? `We have nothing at <span class="font-mono text-foreground">${name}</span>.`
            : "That address does not name a repository."}
          It may have been renamed, never published, or be private to someone else.
        </p>
        <div class="flex flex-wrap items-center justify-center gap-2">
          <a class="dashboard-accent-link hover:underline" href="/dashboard/repos">Your repositories</a>
          <span class="text-muted-foreground" aria-hidden="true">·</span>
          <a class="dashboard-accent-link hover:underline" href="/dashboard/network">Browse the network</a>
        </div>
      </section>`;
  }

  document.addEventListener("click", async (event) => {
    const discussionAnswerButton = event.target.closest("[data-repo-discussion-answer]");
    if (discussionAnswerButton && state.selectedRepo) {
      event.preventDefault();
      void handleDiscussionAnswerClick(state.selectedRepo, discussionAnswerButton);
      return;
    }
    if (event.target.closest("[data-repositories-retry]")) {
      const startedAt = interfaceNow();
      const list = $("#repoList");
      if (list) {
        list.innerHTML = '<div role="status" class="px-4 py-8 text-sm text-muted-foreground sm:px-5"><span class="fm-spinner" aria-hidden="true"></span>Retrying repository list…</div>';
      }
      announceInterfaceStatus("Retrying repository list.");
      recordInterfaceMetric("feedback", startedAt);
      void loadRepositories({ fresh: true });
      return;
    }
    const directMirrorLink = event.target.closest("[data-repo-direct-mirror]");
    if (
      directMirrorLink
      && !event.defaultPrevented
      && event.button === 0
      && !event.metaKey
      && !event.ctrlKey
      && !event.shiftKey
      && !event.altKey
    ) {
      const destination = String(directMirrorLink.href || "");
      let target;
      try { target = new URL(destination); } catch (_) { target = null; }
      const hostname = String(location.hostname || "").toLowerCase();
      const onController = hostname === "forkmesh.com"
        || OFFICIAL_SERVICE_HOSTS.has(hostname);
      if (
        target
        && onController
        && state.session?.sessionToken
        && target.origin !== location.origin
        && /^mirror[0-9]+\.forkmesh\.com$/i.test(target.hostname)
      ) {
        // Keep the visible and final URL direct. The refresh only asks the
        // current App origin to mint the domain-wide, signed HttpOnly grant;
        // the mirror validates it locally and never receives the App bearer.
        event.preventDefault();
        directMirrorLink.setAttribute("aria-busy", "true");
        try {
          await ensurePortableControllerSession(true);
        } finally {
          directMirrorLink.removeAttribute("aria-busy");
        }
        location.assign(destination);
        return;
      }
    }

    const mobileMenuToggle = event.target.closest("[data-mobile-menu-toggle]");
    if (mobileMenuToggle) {
      setMobileSidebarOpen(!document.body.classList.contains("dashboard-sidebar-open"));
      return;
    }

    if (event.target.closest("[data-mobile-sidebar-backdrop]")) {
      setMobileSidebarOpen(false);
      return;
    }

    if (event.target.closest("[data-network-online-only]")) {
      toggleNetworkOnlineOnly();
      return;
    }

    // The header ✕ and the "(clear search)" link inside a filtered-to-nothing
    // list are the same action (adhoc #62): empty every search box and put the
    // rows back. Ahead of the close-the-dropdowns sweep below, which would
    // otherwise close the panel out from under the ✕ before it ran.
    if (event.target.closest("[data-clear-search]")) {
      event.preventDefault();
      clearPageSearchFilters();
      // Only the header control keeps focus — clicking the inline link should
      // leave you looking at the restored list, not at the search box.
      if (event.target.closest("[data-global-search-clear]")) $("[data-global-search]")?.focus();
      return;
    }

    const profileToggle = event.target.closest("[data-profile-toggle]");
    if (profileToggle) {
      const popover = $("[data-profile-popover]");
      const open = popover?.classList.contains("hidden");
      popover?.classList.toggle("hidden", !open);
      profileToggle.setAttribute("aria-expanded", open ? "true" : "false");
      return;
    }

    const notificationToggle = event.target.closest("#notificationToggle");
    if (notificationToggle) {
      event.stopPropagation();
      const dropdown = $("#notificationDropdown");
      setNotificationDropdownOpen(dropdown?.classList.contains("hidden"));
      if (state.session?.nodeName) loadNotifications();
      return;
    }

    const accountMenuToggle = event.target.closest("#accountMenuToggle");
    if (accountMenuToggle) {
      event.stopPropagation();
      const dropdown = $("#accountMenuDropdown");
      setAccountMenuOpen(dropdown?.classList.contains("hidden"));
      return;
    }
    if (event.target.closest("[data-account-menu-status]")) {
      setAccountMenuOpen(false);
      window.ForkMeshUserStatus?.openEditor();
      return;
    }
    if (event.target.closest("[data-account-menu-away]")) {
      setAccountMenuOpen(false);
      const userStatus = window.ForkMeshUserStatus;
      if (userStatus) {
        try {
          await userStatus.setAway(!userStatus.mine().away);
        } catch (error) {
          showErrorNotice(error.message || "Couldn't update your status.");
        }
      }
      return;
    }
    const accountMenuLogout = event.target.closest("[data-account-menu-logout]");
    if (accountMenuLogout) {
      logout();
      return;
    }
    if (event.target.closest("[data-account-menu-logout-all]")) {
      setAccountMenuOpen(false);
      await revokeAllAccountSessions();
      return;
    }

    const mirrorAccept = event.target.closest("[data-mirror-request-accept]");
    if (mirrorAccept) {
      event.stopPropagation();
      await resolveMirrorRequest(mirrorAccept.dataset.mirrorRequestAccept || "", "accept", mirrorAccept);
      return;
    }
    const mirrorReject = event.target.closest("[data-mirror-request-reject]");
    if (mirrorReject) {
      event.stopPropagation();
      await resolveMirrorRequest(mirrorReject.dataset.mirrorRequestReject || "", "reject", mirrorReject);
      return;
    }
    const mirrorAsk = event.target.closest("[data-mirror-request-send]");
    if (mirrorAsk) {
      event.stopPropagation();
      await askNodeToMirror(mirrorAsk);
      return;
    }
    if (event.target.closest("[data-repo-mirror-online-only]")) {
      event.stopPropagation();
      toggleRepoMirrorsOnlineOnly();
      return;
    }
    const mirrorDelete = event.target.closest("[data-repo-mirror-delete]");
    if (mirrorDelete) {
      event.stopPropagation();
      await deleteMirrorNode(mirrorDelete);
      return;
    }

    // Ping settings gear (bell footer, pings modal header, each modal row, the
    // detail pane): a real link to /dashboard/settings/notifications, switched
    // in place when this already is the settings page.
    const pingSettingsLink = event.target.closest("[data-ping-settings-link]");
    if (pingSettingsLink) {
      if (event.metaKey || event.ctrlKey || event.shiftKey || event.altKey || event.button !== 0) return;
      setNotificationDropdownOpen(false);
      setNotificationModalOpen(false);
      if (openPingSettingsInPlace(pingSettingsLink)) event.preventDefault();
      return;
    }

    if (handlePingSettingsClick(event)) {
      event.stopPropagation();
      return;
    }

    const notificationOpen = event.target.closest("[data-notification-open]");
    if (notificationOpen) {
      await openNotification(
        notificationOpen.dataset.notificationOpen || "",
        !event.target.closest("#notificationModal"),
      );
      return;
    }

    const pausePreset = event.target.closest("[data-ping-pause-preset]");
    if (pausePreset) {
      event.stopPropagation();
      await savePingPause(Date.now() + (Number(pausePreset.dataset.pingPausePreset) || 0));
      return;
    }

    if (event.target.closest("[data-ping-pause-custom-apply]")) {
      event.stopPropagation();
      const input = event.target.closest("[data-ping-pause-form]")?.querySelector("[data-ping-pause-custom]");
      // datetime-local reads back in the visitor's own clock, which is the
      // clock they picked against; Date.parse turns it into the instant.
      const picked = Date.parse(input?.value || "");
      if (Number.isFinite(picked)) await savePingPause(picked);
      return;
    }

    if (event.target.closest("[data-ping-pause-resume]")) {
      event.stopPropagation();
      await savePingPause(0);
      return;
    }

    const pauseToggle = event.target.closest("[data-ping-pause-toggle]");
    if (pauseToggle) {
      event.stopPropagation();
      setPingPauseMenuOpen($("[data-ping-pause-menu]")?.hidden !== false);
      return;
    }

    if (event.target.closest("[data-show-all-notifications]")) {
      setNotificationDropdownOpen(false);
      setNotificationModalOpen(true);
      return;
    }

    if (event.target.closest("[data-close-notification-modal], [data-notification-modal-backdrop]")) {
      setNotificationModalOpen(false);
      return;
    }

    if (event.target.closest("#agentModalToggle")) {
      event.stopPropagation();
      setAgentModalOpen($("#agentModal")?.classList.contains("hidden"));
      return;
    }

    if (event.target.closest("[data-agent-modal-close]")) {
      setAgentModalOpen(false);
      return;
    }

    const settingsSectionButton = event.target.closest("[data-settings-section-link]");
    if (settingsSectionButton) {
      setSettingsSection(settingsSectionButton.dataset.settingsSectionLink || "public-profile", { push: true });
      return;
    }

    const sshRevokeButton = event.target.closest("[data-ssh-key-revoke]");
    if (sshRevokeButton) {
      revokeSshKey(sshRevokeButton.dataset.sshKeyRevoke || "");
      return;
    }

    if (event.target.closest("[data-patreon-connect]")) {
      startPatreonOAuth();
      return;
    }

    const appearanceThemeButton = event.target.closest("[data-appearance-theme]");
    if (appearanceThemeButton) {
      saveDashboardTheme(appearanceThemeButton.dataset.appearanceTheme);
      return;
    }

    const genieToggle = event.target.closest("[data-genie-visible]");
    if (genieToggle) {
      saveGenieSetting(genieToggle.getAttribute("aria-checked") !== "true");
      return;
    }

    const homeRedirectToggle = event.target.closest("[data-home-redirect]");
    if (homeRedirectToggle) {
      saveHomeRedirectSetting(homeRedirectToggle.getAttribute("aria-checked") !== "true");
      return;
    }

    if (event.target.closest("[data-profile-modal-close], [data-profile-modal-backdrop]")) {
      setProfileModalOpen(false);
      return;
    }

    if (event.target.closest("[data-profile-about-edit]")) {
      setProfileAboutModalOpen(true);
      return;
    }

    if (event.target.closest("[data-profile-about-modal-close], [data-profile-about-modal-backdrop], [data-profile-about-cancel]")) {
      setProfileAboutModalOpen(false);
      return;
    }

    if (event.target.closest("[data-profile-about-save]")) {
      saveProfileAbout();
      return;
    }

    const contributionPeriod = event.target.closest("[data-profile-contribution-period]");
    if (contributionPeriod) {
      renderProfileContributionGraph({ period: contributionPeriod.dataset.profileContributionPeriod });
      return;
    }

    if (event.target.closest("[data-profile-contribution-retry]")) {
      renderProfileContributionGraph({ force: true });
      return;
    }

    if (event.target.closest("[data-profile-save]")) {
      saveProfile();
      return;
    }

    if (event.target.closest("[data-email-verification-resend]")) {
      resendVerification();
      return;
    }

    if (event.target.closest("[data-profile-verify-email]")) {
      resendVerification({
        hintSelector: "[data-profile-hint]",
        buttonSelector: "[data-profile-verify-email]",
        buttonText: "Send link",
      });
      return;
    }

    if (event.target.closest("[data-profile-page-save]")) {
      saveProfile({
        passwordSelector: "[data-profile-page-password]",
        solanaSelector: "[data-profile-page-solana]",
        hintSelector: "[data-profile-page-hint]",
        buttonSelector: "[data-profile-page-save]",
        buttonText: "Save payout address",
      });
      return;
    }

    if (event.target.closest("[data-profile-public-save]")) {
      savePublicProfile();
      return;
    }

    const avatarEdit = event.target.closest("[data-profile-avatar-edit]");
    if (avatarEdit) {
      const input = $("[data-profile-avatar-input]");
      if (input) {
        input.value = "";
        input.click();
      }
      return;
    }

    if (event.target.closest("[data-profile-page-verify-email]")) {
      resendVerification({
        hintSelector: "[data-profile-page-hint]",
        buttonSelector: "[data-profile-page-verify-email]",
        buttonText: "Send link",
      });
      return;
    }

    if (event.target.closest("[data-admin-test-email]")) {
      sendAdminTestEmail();
      return;
    }

    if (event.target.closest("[data-profile-rename-save]")) {
      renameNodeName();
      return;
    }

    if (event.target.closest("[data-claim-node-send]")) {
      claimNode();
      return;
    }

    if (event.target.closest("[data-claim-code-confirm]")) {
      confirmClaimCode();
      return;
    }

    if (event.target.closest("[data-link-grant-confirm]")) {
      redeemLinkGrant();
      return;
    }

    if (event.target.closest("[data-profile-delete-account]")) {
      deleteAccount();
      return;
    }

    const logoutButton = event.target.closest("[data-logout-button]");
    if (logoutButton) {
      logout();
      return;
    }

    if (!event.target.closest("[data-sidebar-profile]")) {
      $("[data-profile-popover]")?.classList.add("hidden");
      $("[data-profile-toggle]")?.setAttribute("aria-expanded", "false");
    }
    if (!event.target.closest("#notificationToggle, #notificationDropdown")) {
      setNotificationDropdownOpen(false);
    }
    if (!event.target.closest("#accountMenuToggle, #accountMenuDropdown")) {
      setAccountMenuOpen(false);
    }
    if (!event.target.closest("#agentModal, #agentModalToggle")) {
      setAgentModalOpen(false);
    }
    if (!event.target.closest("[data-repo-branch-control]")) {
      closeRepoBranchMenus();
    }
    if (!event.target.closest("[data-global-search-shell]")) {
      closeGlobalSearch();
    }

    const pageButton = event.target.closest("[data-dashboard-repo-page]");
    if (pageButton) {
      state.page = Number(pageButton.dataset.dashboardRepoPage) || 1;
      updateRepositoryPagination();
    }

      const globalSearchResult = event.target.closest("[data-global-search-result]");
      if (globalSearchResult) {
        event.preventDefault();
        selectGlobalSearchResult(globalSearchResult.dataset.dashboardOpenRepo || "");
        return;
      }

      const starTrigger = event.target.closest("[data-repo-action-star], [data-repo-star-button]");
      if (starTrigger) {
        event.preventDefault();
        event.stopPropagation();
        toggleRepoStar(starTrigger);
        return;
      }

      const openButton = event.target.closest("[data-dashboard-open-repo]");
      if (openButton) {
        const nestedControl = event.target.closest("a, button, input, textarea, select, [contenteditable='true']");
        if (nestedControl && nestedControl !== openButton && openButton.contains(nestedControl)) {
          return;
        }
        event.preventDefault();
        // Repo pages are real documents now: opening one is a real navigation.
        openRepoPage(openButton.dataset.dashboardOpenRepo);
        return;
      }

      // Repository tabs, History and Releases are real <a href> links, so
      // middle-click opens them in a new browser tab natively (it fires
      // auxclick, never reaching here). Ctrl/cmd/shift/alt-click does arrive:
      // hand it back to the browser instead of switching tabs in-app.
      const repoTabLink = event.target.closest("[data-dashboard-repo-tab], [data-dashboard-history-button], [data-dashboard-releases-button]");
      if (repoTabLink?.hasAttribute("href") && (event.metaKey || event.ctrlKey
        || event.shiftKey || event.altKey || event.button > 0)) {
        return;
      }

      const historyButton = event.target.closest("[data-dashboard-history-button]");
      if (historyButton) {
        event.preventDefault();
        activateRepoTab("commits");
        return;
      }

      const releasesButton = event.target.closest("[data-dashboard-releases-button]");
      if (releasesButton) {
        event.preventDefault();
        activateRepoTab("releases");
        return;
      }

      if (handleRepoActionsClick(event)) return;

      const repoTabButton = event.target.closest("[data-dashboard-repo-tab]");
      if (repoTabButton) {
        event.preventDefault();
        const tab = repoTabButton.dataset.dashboardRepoTab || "code";
        activateRepoTab(tab);
        return;
      }

      const insightsRefreshButton = event.target.closest("[data-repo-insights-refresh]");
      if (insightsRefreshButton) {
        loadRepoInsights(state.selectedRepo);
        return;
      }

      // Watch popover: the button shows the repo's fediverse follower count
      // and opens the follow-from-Mastodon card; any click outside closes it.
      const watchButton = event.target.closest("[data-repo-action-watch]");
      if (watchButton) {
        $("[data-repo-watch-menu]")?.classList.toggle("hidden");
        return;
      }
      if (!event.target.closest("[data-repo-watch-wrap]")) {
        $("[data-repo-watch-menu]")?.classList.add("hidden");
      }

      const forkCreate = event.target.closest("[data-repo-fork-create]");
      if (forkCreate) {
        event.preventDefault();
        void createRepoForkFromWebsite(forkCreate);
        return;
      }

      // Fork popover: the button shows how many repositories were forked from
      // this one and opens the list of who owns them. Refetched on open so a
      // fork published while the page was sitting there shows up.
      if (event.target.closest("[data-repo-mirror-fork]")) {
        event.preventDefault();
        openRepoForkMenuFromMirror();
        return;
      }
      const forkButton = event.target.closest("[data-repo-action-fork]");
      if (forkButton) {
        const forkMenu = $("[data-repo-fork-menu]");
        const opening = Boolean(forkMenu?.classList.contains("hidden"));
        forkMenu?.classList.toggle("hidden", !opening);
        forkButton.setAttribute("aria-expanded", opening ? "true" : "false");
        if (opening && state.selectedRepo) {
          loadRepoForks(state.selectedRepo);
          void loadRepoRemoteForks(state.selectedRepo, { force: true });
        }
        return;
      }
      const remoteForkRemove = event.target.closest("[data-repo-remote-fork-remove]");
      if (remoteForkRemove && state.selectedRepo) {
        event.preventDefault();
        void saveRepoRemoteFork(state.selectedRepo, "DELETE", remoteForkRemove.dataset.repoRemoteForkRemove || "");
        return;
      }
      if (!event.target.closest("[data-repo-fork-wrap]")) {
        $("[data-repo-fork-menu]")?.classList.add("hidden");
        $("[data-repo-action-fork]")?.setAttribute("aria-expanded", "false");
      }

      // GitHub-style Code dropdown: toggle the clone popover scoped to the
      // clicked button; a click anywhere outside a code wrapper closes them.
      const codeButton = event.target.closest("[data-repo-code-button]");
      if (codeButton) {
        const wrap = codeButton.closest("[data-repo-code-wrap]");
        const menu = wrap?.querySelector("[data-repo-code-menu]");
        const willOpen = Boolean(menu?.classList.contains("hidden"));
        $$("[data-repo-code-menu]").forEach((m) => m.classList.add("hidden"));
        $$("[data-repo-code-button]").forEach((b) => b.setAttribute("aria-expanded", "false"));
        if (menu && willOpen) {
          menu.classList.remove("hidden");
          codeButton.setAttribute("aria-expanded", "true");
          // Pre-select the URL so Ctrl/Cmd+C works immediately, like GitHub.
          wrap.querySelector("[data-repo-clone-url]")?.select?.();
        }
        return;
      }
      if (!event.target.closest("[data-repo-code-wrap]")) {
        $$("[data-repo-code-menu]").forEach((m) => m.classList.add("hidden"));
      }
      if (!event.target.closest("[data-repo-tree-context-menu]")) {
        closeRepoTreeContextMenu();
      }

      const aboutEditButton = event.target.closest("[data-repo-about-edit]");
      if (aboutEditButton && state.selectedRepo && sessionAdministersRepo(state.selectedRepo)) {
        setRepoAboutStatus("");
        setRepoAboutEditing(true);
        return;
      }

      const digestPreviewButton = event.target.closest(
        "[data-repo-digest-preview-refresh]");
      if (digestPreviewButton && state.selectedRepo
          && sessionAdministersRepo(state.selectedRepo)) {
        loadRepoDigestPreview(state.selectedRepo);
        return;
      }

      if (event.target.closest("[data-repo-about-cancel]")) {
        setRepoAboutStatus("");
        setRepoAboutEditing(false);
        return;
      }

      if (await handleRepoForgeMirrorClick(event)) return;

      const webhookRow = event.target.closest("[data-repo-webhook-row]");
      if (
        webhookRow &&
        state.selectedRepo &&
        sessionAdministersRepo(state.selectedRepo)
      ) {
        const repo = state.selectedRepo;
        const id = webhookRow.dataset.repoWebhookId || "";
        const button = event.target.closest("button");
        if (!id || !button) return;
        const run = async (payload, pending) => {
          button.disabled = true;
          setRepoWebhookStatus(pending);
          try {
            return await repoWebhookRequest(repo, { id, ...payload });
          } finally {
            button.disabled = false;
          }
        };
        try {
          if (button.matches("[data-repo-webhook-test]")) {
            const body = await run({ action: "test" }, "Sending test delivery…");
            setRepoWebhookStatus(
              body?.ok
                ? `Test delivered (HTTP ${Number(body.status || 0)}).`
                : `Test not delivered${body?.status ? ` (HTTP ${Number(body.status)})` : ""}.`,
              body?.ok ? "" : "bad",
            );
          } else if (button.matches("[data-repo-webhook-toggle]")) {
            // Read the state from the row, not the button's label: a label is
            // presentation and would silently invert this if it ever changed.
            const paused = webhookRow.dataset.repoWebhookActive === "1";
            await run({ action: "update", active: !paused },
                      paused ? "Pausing…" : "Resuming…");
            setRepoWebhookStatus(paused ? "Webhook paused." : "Webhook resumed.");
          } else if (button.matches("[data-repo-webhook-rotate]")) {
            if (!window.confirm(
              "Rotate this webhook's signing secret? Deliveries signed with the old secret stop verifying as soon as the new one is issued.",
            )) return;
            const body = await run({ action: "rotate" }, "Rotating secret…");
            showRepoWebhookSecret(body?.webhook?.secret || "");
            setRepoWebhookStatus("Secret rotated.");
          } else if (button.matches("[data-repo-webhook-delete]")) {
            if (!window.confirm("Delete this webhook? Queued deliveries are discarded.")) return;
            await run({ action: "delete" }, "Deleting…");
            setRepoWebhookStatus("Webhook deleted.");
          } else {
            return;
          }
        } catch (error) {
          setRepoWebhookStatus(repoWebhookErrorText(error?.message), "bad");
        }
        await loadRepoWebhooks(repo);
        return;
      }

      const deleteRepoButton = event.target.closest("[data-repo-delete]");
      if (
        deleteRepoButton &&
        state.selectedRepo &&
        sessionAdministersRepo(state.selectedRepo)
      ) {
        const repo = state.selectedRepo;
        const expected = repoKey(repo);
        const status = $("[data-repo-delete-status]");
        const confirmation = window.prompt(
          `Type ${expected} to confirm repository deletion.`,
          "",
        );
        if (confirmation === null) return;
        if (String(confirmation).trim() !== expected) {
          if (status) {
            status.textContent = `Confirmation did not match ${expected}.`;
            status.className = "mt-1 text-xs text-destructive";
          }
          return;
        }
        deleteRepoButton.disabled = true;
        if (status) {
          status.textContent = "Deleting repository…";
          status.className = "mt-1 text-xs text-muted-foreground";
        }
        try {
          await deleteRepoFromWeb(repo);
          rememberDeletedRepository(repo);
          location.assign("/dashboard/repos");
        } catch (error) {
          const code = String(error?.message || "");
          if (status) {
            status.textContent =
              code === "not_authorized"
                ? "Only the repository owner or an organization owner or admin can delete this repository."
                : code === "account_required"
                  ? "Sign in as the source node owner first."
                  : "Repository deletion failed. Please try again.";
            status.className = "mt-1 text-xs text-destructive";
          }
          deleteRepoButton.disabled = false;
        }
        return;
      }

      const logoWorkflowSummary = event.target.closest(
        "[data-repo-logo-workflow] > summary",
      );
      if (logoWorkflowSummary && state.selectedRepo) {
        const details = logoWorkflowSummary.parentElement;
        setTimeout(() => {
          if (details?.open) loadRepoLogoSuggestions(state.selectedRepo);
        }, 0);
        return;
      }

      const logoReview = event.target.closest("[data-repo-logo-review]");
      if (logoReview && state.selectedRepo) {
        const action = logoReview.getAttribute("data-repo-logo-review") || "";
        const suggestionId =
          logoReview.getAttribute("data-repo-logo-suggestion-id") || "";
        logoReview.disabled = true;
        setRepoLogoSuggestionStatus(
          action === "approve" ? "Approving…" : "Rejecting…",
        );
        reviewRepoLogoSuggestion(state.selectedRepo, suggestionId, action)
          .then(() =>
            setRepoLogoSuggestionStatus(
              action === "approve" ? "Official logo updated." : "Suggestion rejected.",
              "good",
            ),
          )
          .catch(() =>
            setRepoLogoSuggestionStatus(
              "Only the repository owner or a moderator can review suggestions.",
              "bad",
            ),
          )
          .finally(() => {
            logoReview.disabled = false;
          });
        return;
      }

      // Owner "Fediverse posts" dropdown: load the post list the moment it's
      // expanded (setTimeout so the <details> default toggle has applied), and
      // delete a post from its trash button.
      const fediPostsSummary = event.target.closest("[data-repo-fedi-posts] > summary");
      if (fediPostsSummary && state.selectedRepo) {
        const repo = state.selectedRepo;
        const details = fediPostsSummary.parentElement;
        setTimeout(() => { if (details.open) loadRepoFediPosts(repo); }, 0);
        return;
      }

      const fediPostDelete = event.target.closest("[data-repo-fedi-post-delete]");
      if (fediPostDelete && state.selectedRepo) {
        removeRepoFediPost(
          state.selectedRepo,
          fediPostDelete.getAttribute("data-repo-fedi-post-delete"),
          fediPostDelete);
        return;
      }

      // Links inside rendered markdown that point back into the repository
      // (or at a heading of the same document) open in place; modified clicks
      // keep the browser's new-tab behaviour.
      const markdownLink = event.target.closest("a[data-repo-markdown-link], a[data-repo-markdown-anchor]");
      if (markdownLink && state.selectedRepo && event.button === 0
          && !event.metaKey && !event.ctrlKey && !event.shiftKey && !event.altKey) {
        event.preventDefault();
        openRepoMarkdownLink(state.selectedRepo, markdownLink);
        return;
      }

      const readmeLink = event.target.closest("[data-repo-readme-link]");
      if (readmeLink && state.selectedRepo) {
        event.preventDefault();
        setRepoTab("code");
        loadRepositoryBlob(state.selectedRepo, readmeLink.dataset.repoReadmePath || "README.md");
        return;
      }

      const activityLink = event.target.closest("[data-repo-activity-link]");
      if (activityLink && state.selectedRepo) {
        event.preventDefault();
        activateRepoTab("insights");
        return;
      }

      const commitButton = event.target.closest("[data-dashboard-commit-hash]");
      if (commitButton && state.selectedRepo) {
        // A hash clicked outside the Commits tab (a branch tip on the
        // Branches page) has to bring that tab forward, because the detail
        // renders into its panel. Marking it loaded keeps activateRepoTab
        // from starting a list fetch that would race this detail into place.
        if (state.activeRepoTab !== "commits") {
          state.loadedRepoTabs = state.loadedRepoTabs || {};
          state.loadedRepoTabs.commits = true;
          activateRepoTab("commits");
        }
        const commitHash = commitButton.dataset.dashboardCommitHash || "";
        if (commitHash) navigateHistory(repoCommitUrl(state.selectedRepo, commitHash));
        loadRepoCommitDetail(state.selectedRepo, commitHash);
        return;
      }

      if (event.target.closest("[data-repo-commit-back]") && state.selectedRepo) {
        navigateHistory(`${repoLinkUrl(state.selectedRepo)}/commits`);
        state.repoCommitDetail = null;
        loadRepoCommits(state.selectedRepo);
        return;
      }

      if (event.target.closest("[data-repo-branch-close]")) {
        closeRepoBranchMenus();
        return;
      }

      const branchesLink = event.target.closest("[data-repo-branches-open], [data-repo-branch-view-all]");
      if (branchesLink && state.selectedRepo) {
        event.preventDefault();
        closeRepoBranchMenus();
        setRepoBranchQuery(state.selectedRepo, "");
        activateRepoTab("branches");
        return;
      }

      // Branch management (adhoc #2098). Checked before the browse handler
      // below: these controls sit in the same row as the branch name, and a
      // delete must never be read as "browse this branch instead".
      const branchSelect = event.target.closest("[data-repo-branch-select]");
      if (branchSelect && state.selectedRepo) {
        setRepoBranchSelected(state.selectedRepo, branchSelect.dataset.repoBranchSelect || "", branchSelect.checked);
        return;
      }

      const branchDelete = event.target.closest("[data-repo-branch-delete]");
      if (branchDelete && state.selectedRepo) {
        setRepoBranchConfirm(state.selectedRepo, { kind: "one", branch: branchDelete.dataset.repoBranchTarget || "" });
        return;
      }

      const branchDeleteConfirm = event.target.closest("[data-repo-branch-delete-confirm]");
      if (branchDeleteConfirm && state.selectedRepo) {
        await confirmRepoBranchDelete(state.selectedRepo, branchDeleteConfirm.dataset.repoBranchTarget || "");
        return;
      }

      if (event.target.closest("[data-repo-branch-delete-cancel], [data-repo-branches-delete-selected-cancel]") && state.selectedRepo) {
        setRepoBranchConfirm(state.selectedRepo, null);
        return;
      }

      if (event.target.closest("[data-repo-branches-delete-selected]") && state.selectedRepo) {
        setRepoBranchConfirm(state.selectedRepo, { kind: "batch" });
        return;
      }

      if (event.target.closest("[data-repo-branches-delete-selected-confirm]") && state.selectedRepo) {
        await confirmRepoBranchesDeleteSelected(state.selectedRepo);
        return;
      }

      if (event.target.closest("[data-repo-branches-select-clear]") && state.selectedRepo) {
        repoBranchSelection(state.selectedRepo).clear();
        setRepoBranchConfirm(state.selectedRepo, null);
        return;
      }

      if (event.target.closest("[data-repo-branches-filter-merged]") && state.selectedRepo) {
        toggleRepoBranchMergedFilter(state.selectedRepo);
        return;
      }

      if (event.target.closest("[data-repo-branch-delete-results-clear]") && state.selectedRepo) {
        clearRepoBranchDeleteResults(state.selectedRepo);
        return;
      }

      const branchUpdate = event.target.closest("[data-repo-branch-update]");
      if (branchUpdate && state.selectedRepo) {
        await updateRepoBranchFromDefault(
          state.selectedRepo,
          branchUpdate.dataset.repoBranchTarget || "",
          branchUpdate.dataset.repoBranchOid || "",
        );
        return;
      }

      const branchMerge = event.target.closest("[data-repo-branch-merge]");
      if (branchMerge && state.selectedRepo) {
        await openRepoBranchMergeDialog(
          state.selectedRepo,
          branchMerge.dataset.repoBranchTarget || "",
          repoDefaultBranch(state.selectedRepo),
        );
        return;
      }

      if (event.target.closest("[data-repo-branch-open-actions]") && state.selectedRepo) {
        activateRepoTab("actions");
        return;
      }

      const branchOutcomeCompare = event.target.closest("[data-repo-branch-outcome-compare]");
      if (branchOutcomeCompare && state.selectedRepo) {
        const base = branchOutcomeCompare.dataset.repoBranchOutcomeBase || repoDefaultBranch(state.selectedRepo);
        const head = branchOutcomeCompare.dataset.repoBranchOutcomeHead || "";
        if (head && head !== base) await openRepoCompare(state.selectedRepo, base, head);
        return;
      }

      if (event.target.closest("[data-repo-branch-results-clear]") && state.selectedRepo) {
        await clearRepoBranchResults(state.selectedRepo);
        return;
      }

      const branchOpCancel = event.target.closest("[data-repo-branch-op-cancel]");
      if (branchOpCancel && state.selectedRepo) {
        await cancelRepoBranchOp(
          state.selectedRepo, branchOpCancel.dataset.repoBranchOpId || "");
        return;
      }

      if (event.target.closest("[data-repo-branch-create]") && state.selectedRepo) {
        await createRepoBranch(state.selectedRepo);
        return;
      }

      if (event.target.closest("[data-repo-branch-update-all]") && state.selectedRepo) {
        await updateAllRepoBranchesFromDefault(state.selectedRepo);
        return;
      }

      if (event.target.closest("[data-repo-branches-refresh]") && state.selectedRepo) {
        await loadRepoBranches(state.selectedRepo, { refresh: true });
        return;
      }

      const branchItem = event.target.closest("[data-repo-branch-name]");
      if (branchItem && state.selectedRepo) {
        setRepoSelectedBranch(state.selectedRepo, branchItem.dataset.repoBranchName || "");
        setRepoBranchQuery(state.selectedRepo, "");
        resetRepoFileFinder(state.selectedRepo);
        // Chosen from the Branches page, "browse this branch" means the code
        // tab has to come forward - that is where the tree renders. The tree
        // is marked loaded first so activateRepoTab doesn't fetch it once and
        // the explicit load below fetch it again.
        if (state.activeRepoTab === "branches") {
          state.loadedRepoTabs = state.loadedRepoTabs || {};
          state.loadedRepoTabs.code = true;
          activateRepoTab("code");
        }
        updateRepoBranchControls(state.selectedRepo, null);
        loadRepositoryTree(state.selectedRepo, "");
        loadRepoFeaturePanels(state.selectedRepo);
        return;
      }

      const branchButton = event.target.closest("[data-repo-branch-button]");
      if (branchButton && state.selectedRepo) {
        await toggleRepoBranchMenu(state.selectedRepo, branchButton);
        return;
      }

      const codeMapButton = event.target.closest("[data-repo-codemap-open]");
      if (codeMapButton && state.selectedRepo) {
        openRepoCodeMap(state.selectedRepo, codeMapButton);
        return;
      }

      const fileFinderOpenButton = event.target.closest("[data-repo-file-finder-open]");
      if (fileFinderOpenButton) {
        openRepoFileFinder(fileFinderOpenButton);
        return;
      }

      if (event.target.closest("[data-repo-file-finder-close], [data-repo-file-finder-backdrop]")) {
        closeRepoFileFinder();
        return;
      }

      const fileFinderResult = event.target.closest("[data-repo-file-finder-result]");
      if (fileFinderResult && state.selectedRepo) {
        state.repoFileFinder.selectedIndex = $$('[data-repo-file-finder-result]').indexOf(fileFinderResult);
        closeRepoFileFinder();
        loadRepositoryBlob(state.selectedRepo, fileFinderResult.dataset.repoFileFinderPath || "");
        return;
      }

      const treePathButton = event.target.closest("[data-dashboard-tree-path]");
      if (treePathButton && state.selectedRepo) {
        loadRepositoryTree(state.selectedRepo, treePathButton.dataset.dashboardTreePath || "");
        return;
      }

      const blobPathButton = event.target.closest("[data-dashboard-blob-path]");
      if (blobPathButton && state.selectedRepo) {
        loadRepositoryBlob(state.selectedRepo, blobPathButton.dataset.dashboardBlobPath || "");
        return;
      }

      const issueNewMenu = event.target.closest("[data-repo-issue-new-menu]");
      if (issueNewMenu && event.target.closest("[data-repo-issue-import], [data-repo-issue-template]")) {
        issueNewMenu.open = false;
      }

      const issueNewButton = event.target.closest("[data-repo-issue-new]");
      if (issueNewButton && state.selectedRepo) {
        if (!state.session?.nodeName) {
          location.href = "/login?next=" + encodeURIComponent(`${location.pathname}${location.search}`);
          return;
        }
        openIssueCompose(state.selectedRepo);
        return;
      }

      const issueImportButton = event.target.closest("[data-repo-issue-import]");
      if (issueImportButton && state.selectedRepo) {
        if (!state.session?.nodeName) {
          location.href = "/login?next=" + encodeURIComponent(`${location.pathname}${location.search}`);
          return;
        }
        openIssueImport(state.selectedRepo);
        return;
      }

      const contributionDismissButton = event.target.closest(
        "[data-repo-issue-contribution-dismiss]",
      );
      if (contributionDismissButton && state.selectedRepo) {
        const scope = contributionDismissButton.dataset.repoIssueContributionDismiss;
        try {
          const key = scope === "all"
            ? "forkmesh.issue-contribution-tip.v1:all"
            : issueContributionTipStorageKey(state.selectedRepo);
          localStorage.setItem(key, "1");
        } catch (_) {
          /* storage unavailable: still dismiss this rendered tip */
        }
        contributionDismissButton.closest("[data-repo-issue-contribution-tip]")?.remove();
        return;
      }

      const issueTemplateButton = event.target.closest("[data-repo-issue-template]");
      if (issueTemplateButton) {
        downloadIssueCsvTemplate();
        return;
      }

      if (event.target.closest("[data-repo-contribute]") && state.selectedRepo) {
        event.preventDefault();
        if (!state.session?.nodeName) {
          location.href = "/login?next=" + encodeURIComponent(`${location.pathname}${location.search}`);
          return;
        }
        const forkRepo = state.selectedRepo;
        activateRepoTab("pulls");
        requestAnimationFrame(() => openPullCompose(forkRepo));
        return;
      }

      const pullNewButton = event.target.closest("[data-repo-pull-new]");
      if (pullNewButton && state.selectedRepo && !repoReadOnlyMirror(state.selectedRepo)) {
        if (!state.session?.nodeName) {
          location.href = "/login?next=" + encodeURIComponent(`${location.pathname}${location.search}`);
          return;
        }
        openPullCompose(state.selectedRepo);
        return;
      }

      const pullBatchOpen = event.target.closest("[data-repo-pull-batch-open]");
      if (pullBatchOpen && state.selectedRepo && !event.metaKey && !event.ctrlKey && !event.shiftKey) {
        event.preventDefault();
        void openPullBatch(state.selectedRepo);
        return;
      }

      const pullCancelButton = event.target.closest("[data-repo-pull-cancel]");
      if (pullCancelButton && state.selectedRepo) {
        // The form may have been opened from a comparison, whose address is
        // still showing; the list is the page this returns to.
        navigateHistory(`${repoLinkUrl(state.selectedRepo)}/pulls`);
        loadRepoCollection(state.selectedRepo, "pulls", "[data-repo-pulls]");
        return;
      }

      // The branch comparison page (openRepoCompare): back to the list, a
      // re-read, or the new-pull form pre-filled with the compared pair.
      const compareBackButton = event.target.closest("[data-repo-compare-back]");
      if (compareBackButton && state.selectedRepo) {
        state.repoCompare = null;
        navigateHistory(`${repoLinkUrl(state.selectedRepo)}/pulls`);
        loadRepoCollection(state.selectedRepo, "pulls", "[data-repo-pulls]");
        return;
      }
      const compareRefreshButton = event.target.closest("[data-repo-compare-refresh]");
      if (compareRefreshButton && state.selectedRepo && state.repoCompare) {
        const { base, head } = state.repoCompare;
        openRepoCompare(state.selectedRepo, base, head, { navigate: false });
        return;
      }
      if (event.target.closest("[data-repo-compare-merge-open]") && state.selectedRepo && state.repoCompare) {
        const { base, head } = state.repoCompare;
        await openRepoBranchMergeDialog(
          state.selectedRepo, head, base, state.repoCompare.merge?.preview || null);
        return;
      }
      if (event.target.closest("[data-repo-compare-update]") && state.selectedRepo && state.repoCompare) {
        const { base, head } = state.repoCompare;
        const preview = state.repoCompare.merge?.preview;
        await updateRepoBranchFrom(
          state.selectedRepo, head, base, preview && !preview.error ? preview.headOid : "");
        return;
      }
      const comparePullButton = event.target.closest("[data-repo-compare-pull]");
      if (comparePullButton && state.selectedRepo && state.repoCompare && !repoReadOnlyMirror(state.selectedRepo)) {
        if (!state.session?.nodeName) {
          location.href = "/login?next=" + encodeURIComponent(`${location.pathname}${location.search}`);
          return;
        }
        const { base, head } = state.repoCompare;
        navigateHistory(`${repoLinkUrl(state.selectedRepo)}/pulls`);
        openPullCompose(state.selectedRepo, { base, head });
        return;
      }

      const issueFilterButton = event.target.closest("[data-dashboard-issue-filter]");
      if (issueFilterButton) {
        setIssueFilter(issueFilterButton.dataset.dashboardIssueFilter || "open");
        return;
      }

      const discussionCategory = event.target.closest("[data-repo-discussion-category]");
      if (discussionCategory) {
        setDiscussionFilter("category", discussionCategory.dataset.repoDiscussionCategory || "");
        return;
      }

      const pullViewButton = event.target.closest("[data-repo-pull-view]");
      if (pullViewButton) {
        setPullView(pullViewButton.dataset.repoPullView || "pulls");
        return;
      }

      const pullBaseView = event.target.closest("[data-repo-pull-base-view]");
      if (pullBaseView) {
        const value = pullBaseView.dataset.repoPullBaseView || "";
        const current = String(pullQueryFilters().base || "");
        setPullCollectionFilter("base", current.toLowerCase() === value.toLowerCase() ? "" : value);
        return;
      }

      const pullTipDismiss = event.target.closest("[data-repo-pull-contribution-dismiss]");
      if (pullTipDismiss && state.selectedRepo) {
        try {
          localStorage.setItem(`forkmesh.pull-contribution-tip.v1:${repoKey(state.selectedRepo)}`, "1");
        } catch (_) {
          /* storage unavailable: still dismiss this rendered tip */
        }
        pullTipDismiss.closest("[data-repo-pull-contribution-tip]")?.remove();
        return;
      }

      const issueViewButton = event.target.closest("[data-repo-issue-view]");
      if (issueViewButton) {
        setIssueView(issueViewButton.dataset.repoIssueView || "issues");
        return;
      }

      // Sidebar labels and milestones toggle a filter on top of the view.
      const issueLabelView = event.target.closest("[data-repo-issue-label-view]");
      const issueMilestoneView = event.target.closest("[data-repo-issue-milestone-view]");
      if (issueLabelView || issueMilestoneView) {
        const field = issueLabelView ? "label" : "milestone";
        const value = (issueLabelView || issueMilestoneView).dataset[
          issueLabelView ? "repoIssueLabelView" : "repoIssueMilestoneView"] || "";
        const current = String(issueFilterState()[field] || "");
        setIssueCollectionFilter(field, current.toLowerCase() === value.toLowerCase() ? "" : value);
        return;
      }

      if (event.target.closest("[data-repo-issue-views-toggle]")) {
        setIssueViewsCollapsed(!issueViewsCollapsed());
        window.lucide?.createIcons();
        return;
      }

      const issueQuerySet = event.target.closest("[data-repo-issue-query-set]");
      if (issueQuerySet) {
        applyIssueQueryText(`is:issue ${issueQuerySet.dataset.repoIssueQuerySet || ""}`);
        return;
      }

      const issuesReloadButton = event.target.closest(
        "[data-repo-issues-refresh], [data-repo-issues-reload]",
      );
      if (issuesReloadButton && state.selectedRepo) {
        loadRepoIssues(state.selectedRepo);
        return;
      }

      const projectFilterButton = event.target.closest("[data-dashboard-project-filter]");
      if (projectFilterButton) {
        setProjectFilter(projectFilterButton.dataset.dashboardProjectFilter || "open");
        return;
      }

      const projectViewButton = event.target.closest("[data-dashboard-project-view]");
      if (projectViewButton) {
        setProjectView(projectViewButton.dataset.dashboardProjectView || "gantt");
        return;
      }

      const issueCancelButton = event.target.closest("[data-repo-issue-cancel]");
      if (issueCancelButton && state.selectedRepo) {
        renderRepoIssues();
        return;
      }

      const recordButton = event.target.closest("[data-repo-record-kind][data-repo-record-number]");
      if (recordButton && state.selectedRepo) {
        // List rows are real <a href> links so a record can be opened in its
        // own tab. Middle-click never reaches this listener (browsers fire
        // auxclick for it, and the anchor handles it natively), but
        // ctrl/cmd/shift/alt-click does: hand those back to the browser rather
        // than swallowing them into in-app routing. Everything else is a plain
        // left click, which the anchor would turn into a full page load, so
        // suppress the navigation and keep routing in-app.
        const opensNewContext = event.metaKey || event.ctrlKey || event.shiftKey
          || event.altKey || event.button > 0;
        if (recordButton.hasAttribute("href")) {
          if (opensNewContext) return;
          event.preventDefault();
        }
        const kind = recordButton.dataset.repoRecordKind || "";
        const number = recordButton.dataset.repoRecordNumber || "";
        // Mirror the opened record into the address bar (/owner/repo/pulls/4)
        // so refresh and the desktop client's "View on website" button land on
        // this same detail page. Pending records have no mirror number yet.
        // Opening a record from the list always starts on Conversation.
        state.repoRecordSection = "conversation";
        if (["pulls", "discussions", "issues"].includes(kind) && /^\d+$/.test(number)) {
          navigateHistory(repoRecordUrl(state.selectedRepo, kind, number, "conversation"));
        }
        loadRepoRecordDetail(state.selectedRepo, kind, number);
        return;
      }

      const recordBackButton = event.target.closest("[data-repo-record-back]");
      if (recordBackButton && state.selectedRepo) {
        const kind = recordBackButton.dataset.repoRecordBack || "";
        if (["pulls", "discussions", "issues"].includes(kind)) {
          navigateHistory(`${repoLinkUrl(state.selectedRepo)}/${kind}`);
        }
        if (kind === "pulls") stopRepoPullActivityRefresh();
        state.repoRecordDetail = null;
        state.repoRecordSection = "conversation";
        if (kind === "issues") {
          // A deep-linked refresh straight into the issue detail never loaded
          // the list, so fetch it now instead of flashing an empty "No issues".
          if (state.issuesView.items.length) renderRepoIssues();
          else loadRepoIssues(state.selectedRepo);
        } else {
          loadRepoCollection(state.selectedRepo, kind, `[data-repo-${kind}]`);
        }
        return;
      }

      const pickerOpen = event.target.closest("[data-repo-issue-picker-open]");
      if (pickerOpen) {
        const field = pickerOpen.dataset.repoIssuePickerOpen || "";
        if ($(`[data-repo-issue-picker="${field}"]`)) closeIssuePicker({ apply: true });
        else openIssuePicker(field);
        return;
      }
      const pickerOption = event.target.closest("[data-repo-issue-picker-option]");
      if (pickerOption) {
        toggleIssuePickerOption(pickerOption.dataset.repoIssuePickerOption || "");
        return;
      }
      if (event.target.closest("[data-repo-issue-picker-close]")) {
        closeIssuePicker({ apply: true });
        return;
      }
      if (event.target.closest("[data-repo-issue-picker-apply]")) {
        applyIssueFieldsPicker();
        return;
      }

      const commentTab = event.target.closest("[data-repo-issue-comment-tab]");
      if (commentTab) {
        setIssueCommentTab(commentTab.closest("[data-repo-issue-comment-form]"),
          commentTab.dataset.repoIssueCommentTab || "write");
        return;
      }

      const commentFormat = event.target.closest("[data-repo-issue-comment-format]");
      if (commentFormat) {
        applyIssueCommentFormat(
          commentFormat.closest("[data-repo-issue-comment-form]")?.querySelector("[data-repo-issue-comment-body]"),
          commentFormat.dataset.repoIssueCommentFormat || "");
        return;
      }

      const issueMcpPromptButton = event.target.closest(
        "[data-repo-issue-mcp-prompt]",
      );
      if (issueMcpPromptButton) {
        // A plain click copies straight away with a generated connector token.
        // Shift-click opens the paste box instead, so a node that already
        // published its own connector can hand over that token by hand.
        void copyIssueMcpPrompt(issueMcpPromptButton, {
          replaceToken: event.shiftKey === true,
        });
        return;
      }

      const issueTitleEditButton = event.target.closest(
        "[data-repo-issue-title-edit]",
      );
      if (issueTitleEditButton) {
        const article = issueTitleEditButton.closest("[data-repo-record-detail]");
        article?.querySelector("[data-repo-issue-title-heading]")?.classList.add("hidden");
        issueTitleEditButton.classList.add("hidden");
        const form = article?.querySelector("[data-repo-issue-title-form]");
        form?.classList.remove("hidden");
        form?.classList.add("flex");
        form?.querySelector("[data-repo-issue-title-input]")?.focus();
        return;
      }

      const issueTitleCancelButton = event.target.closest(
        "[data-repo-issue-title-cancel]",
      );
      if (issueTitleCancelButton) {
        renderCurrentWebIssueDetail();
        return;
      }

      const issueDescriptionEditButton = event.target.closest(
        "[data-repo-issue-description-edit]",
      );
      if (issueDescriptionEditButton) {
        const section = issueDescriptionEditButton.closest("[data-repo-record-conversation]");
        section?.querySelector("[data-repo-record-body]")?.classList.add("hidden");
        issueDescriptionEditButton.parentElement?.classList.add("hidden");
        section?.querySelector("[data-repo-issue-description-form]")?.classList.remove("hidden");
        section?.querySelector("[data-repo-issue-description-input]")?.focus();
        return;
      }

      if (event.target.closest("[data-repo-issue-description-cancel]")) {
        renderCurrentWebIssueDetail();
        return;
      }

      // The handle's click is intentionally quiet; mouse drag or the Arrow
      // keys perform the reorder without opening another issue action.
      if (event.target.closest("[data-repo-issue-task-drag]")) return;

      const addSubIssueButton = event.target.closest("[data-repo-issue-add-subissue]");
      if (addSubIssueButton && state.selectedRepo) {
        if (!state.session?.nodeName) {
          location.href = "/login?next=" + encodeURIComponent(`${location.pathname}${location.search}`);
          return;
        }
        openIssueCompose(state.selectedRepo, {
          parentNumber: Number(addSubIssueButton.dataset.repoIssueAddSubissue) || 0,
        });
        return;
      }

      const issueSubtasksButton = event.target.closest(
        "[data-repo-issue-create-subissues]",
      );
      if (issueSubtasksButton) {
        void handleWebIssueSubtasks(issueSubtasksButton);
        return;
      }

      if (event.target.closest("[data-repo-issue-vote]")) {
        void handleWebIssueAction("vote");
        return;
      }

      const issueClaimButton = event.target.closest("[data-repo-issue-claim]");
      if (issueClaimButton) {
        void handleWebIssueAction(
          issueClaimButton.dataset.repoIssueClaim || "claim",
        );
        return;
      }

      const issueSubscriptionButton = event.target.closest(
        "[data-repo-issue-subscription]",
      );
      if (issueSubscriptionButton) {
        void handleWebIssueAction(
          issueSubscriptionButton.dataset.repoIssueSubscription || "subscribe",
        );
        return;
      }

      if (event.target.closest("[data-repo-issue-move-to-task]")) {
        void moveWebIssueToTask(event.target.closest("[data-repo-issue-move-to-task]"));
        return;
      }

      if (event.target.closest("[data-repo-issue-delete]")) {
        void handleWebIssueAction("delete-issue");
        return;
      }

      const issueCommentEditButton = event.target.closest(
        "[data-repo-issue-comment-edit]",
      );
      if (issueCommentEditButton) {
        void handleWebIssueAction(
          "edit-comment",
          issueCommentEditButton.dataset.repoIssueCommentEdit || "",
        );
        return;
      }

      const issueCommentDeleteButton = event.target.closest(
        "[data-repo-issue-comment-delete]",
      );
      if (issueCommentDeleteButton) {
        void handleWebIssueAction(
          "delete-comment",
          issueCommentDeleteButton.dataset.repoIssueCommentDelete || "",
        );
        return;
      }

      const inlineThreadButton = event.target.closest("[data-repo-pull-inline-thread-toggle]");
      if (inlineThreadButton) {
        const key = inlineThreadButton.dataset.repoPullInlineThreadToggle || "";
        const open = !state.repoPullInlineOpenThreads.has(key);
        if (open) state.repoPullInlineOpenThreads.add(key);
        else state.repoPullInlineOpenThreads.delete(key);
        inlineThreadButton.setAttribute("aria-expanded", open ? "true" : "false");
        Array.from(document.querySelectorAll("[data-repo-pull-inline-thread]")).forEach((thread) => {
          if (thread.dataset.repoPullInlineThread === key) thread.classList.toggle("hidden", !open);
        });
        return;
      }

      const inlineCancelButton = event.target.closest("[data-repo-pull-inline-cancel]");
      if (inlineCancelButton) {
        inlineCancelButton.closest("[data-repo-pull-inline-composer]")?.remove();
        clearPullDiffSelection();
        return;
      }

      const draftDiscardButton = event.target.closest("[data-repo-pull-review-draft-discard]");
      if (draftDiscardButton) {
        discardPullReviewDraft(draftDiscardButton.dataset.repoPullReviewDraftDiscard || "");
        return;
      }

      if (event.target.closest("[data-repo-pull-review-batch-discard]")) {
        discardPullReviewDraft();
        return;
      }

      if (event.target.closest("[data-repo-pull-review-batch-submit]") && state.selectedRepo) {
        void submitPullReviewBatch(state.selectedRepo);
        return;
      }

      // A mouse selects on pointerdown/up (below); the click that follows is
      // spent. Keyboard activation (detail 0) and a finger's tap land here.
      const clickedDiffLine = event.target.closest("[data-repo-pull-diff-line]");
      if (clickedDiffLine && event.detail === 0) {
        if (beginPullDiffSelection(clickedDiffLine)) finishPullDiffSelection();
        return;
      }
      if (clickedDiffLine) {
        if (lastPullDiffLinePointerType !== "mouse") tapPullDiffLine(clickedDiffLine);
        return;
      }

      const pullViewedButton = event.target.closest("[data-repo-pull-viewed]");
      if (pullViewedButton && state.selectedRepo) {
        keepPullDiffHeaderInPlace(pullViewedButton, () => toggleRepoPullViewed(
          state.selectedRepo,
          pullViewedButton.dataset.repoPullViewed || "",
        ));
        return;
      }

      const pullDiffCollapseButton = event.target.closest("[data-repo-pull-diff-collapse]");
      if (pullDiffCollapseButton) {
        const block = pullDiffCollapseButton.closest("[data-repo-pull-diff-file]");
        if (block) {
          const collapsed = block.dataset.collapsed !== "true";
          keepPullDiffHeaderInPlace(pullDiffCollapseButton, () => {
            setRepoPullDiffBlockCollapsed(block, collapsed);
            pullDiffCollapseButton.setAttribute("aria-expanded", collapsed ? "false" : "true");
          });
          window.lucide?.createIcons();
        }
        return;
      }

      const pullExplorerModeButton = event.target.closest("[data-repo-pull-explorer-mode]");
      if (pullExplorerModeButton) {
        setRepoPullExplorerMode(pullExplorerModeButton.dataset.repoPullExplorerMode || "tree");
        return;
      }

      const pullDiffModeButton = event.target.closest("[data-repo-pull-diff-mode]");
      if (pullDiffModeButton) {
        setRepoPullDiffMode(pullDiffModeButton.dataset.repoPullDiffMode || "unified");
        return;
      }

      const pullFileButton = event.target.closest("[data-repo-pull-file]");
      if (pullFileButton) {
        const path = pullFileButton.dataset.repoPullFile || "";
        const section = pullFileButton.closest("[data-repo-pull-files]");
        // Scroll-spy would otherwise fight the jump it just triggered.
        if (section) section.dataset.activeFile = path;
        setRepoPullActiveFile(path, { revealDiff: true });
        return;
      }

      const pullMergeButton = event.target.closest("[data-repo-pull-merge]");
      if (pullMergeButton && state.selectedRepo) {
        handleRepoPullMerge(state.selectedRepo);
        return;
      }

      const pullUpdateButton = event.target.closest("[data-repo-pull-update]");
      if (pullUpdateButton && state.selectedRepo) {
        handleRepoPullUpdate(state.selectedRepo);
        return;
      }

      // PR detail section tabs are isolated panels, matching the GitHub review
      // surface: only the selected section is visible at a time.
      const recordTabButton = event.target.closest("[data-repo-record-tab]");
      if (recordTabButton) {
        const article = recordTabButton.closest("[data-repo-record-detail]");
        if (article) {
          setRepoRecordSection(recordTabButton.dataset.repoRecordTab || "", article);
          // Sections are addressable: put the open tab in the address bar so
          // the link a reviewer copies from "Files changed" reopens there for
          // whoever they send it to (and survives a refresh).
          const detail = state.repoRecordDetail;
          if (detail && detail.kind === "pulls" && state.selectedRepo) {
            navigateHistory(repoRecordUrl(
              state.selectedRepo, detail.kind, detail.number,
              state.repoRecordSection));
          }
        }
        return;
      }

      const pullFilterButton = event.target.closest("[data-dashboard-pull-filter]");
      if (pullFilterButton && state.selectedRepo) {
        setPullFilter(pullFilterButton.dataset.dashboardPullFilter || "open");
        return;
      }

      const repoCollectionPageButton = event.target.closest("[data-repo-collection-page]");
      if (repoCollectionPageButton && state.selectedRepo) {
        const kind = repoCollectionPageButton.dataset.repoCollectionPage;
        const targetPage = Number(repoCollectionPageButton.dataset.repoCollectionPageTarget);
        if (kind && Number.isFinite(targetPage)) {
          state.repoCollectionPages[kind] = targetPage;
          // Issues and pulls keep their state filters in memory. Reloading a
          // collection here would replace its filtered page with a fresh raw
          // list, so paginate the cached renderer instead.
          if (kind === "issues") renderRepoIssues();
          else if (kind === "pulls") renderRepoCollection("pulls");
          else loadRepoCollection(state.selectedRepo, kind, `[data-repo-${kind}]`);
        }
        return;
      }

      const copyButton = event.target.closest("[data-dashboard-copy]");
      if (copyButton) {
        const startedAt = interfaceNow();
        const text = copyButton.dataset.dashboardCopy || "";
        const copied = await copyTextToClipboard(text);
        if (copied) {
          copyButton.classList.add("copied");
          if (copyButton.closest("[data-repo-code-wrap]")) {
            window.ForkMeshAnalytics?.recordProductAction?.("repository-clone-copied");
            // First-party acquisition signal (docs/attribution.md). The
            // server joins it to the HttpOnly fm_attr session cookie; only
            // the repository name travels, never the URL that was copied.
            try {
              const repo = state.selectedRepo;
              fetch("/api/attribution/events", {
                method: "POST",
                credentials: "same-origin",
                keepalive: true,
                headers: { "content-type": "application/json" },
                body: JSON.stringify({
                  event: "clone_url_copied",
                  repo: repo ? `${repo.owner || ""}/${repo.name || ""}` : "",
                }),
              }).catch(() => {});
            } catch (_) {
              // Attribution is never allowed to interfere with the copy.
            }
          }
          window.setTimeout(() => copyButton.classList.remove("copied"), 1600);
        }
        announceInterfaceStatus(copied
          ? `${copyButton.getAttribute("aria-label") || "Command"} copied.`
          : "Could not copy. Select the command and copy it manually.");
        recordInterfaceMetric("feedback", startedAt);
        return;
      }

      const contextMenuAction = event.target.closest("[data-repo-tree-context-action]");
      if (contextMenuAction) {
        await handleRepoTreeContextAction(contextMenuAction);
        return;
      }

      const agentsRefreshButton = event.target.closest("[data-repo-agents-refresh]");
      if (agentsRefreshButton && state.selectedRepo) {
        // On the detail page, Refresh reloads that agent's transcript; on the
        // list it reloads the session list (adhoc #259). The org panel has no
        // transcript element of its own - its open session is redrawn from the
        // session read - so there Refresh must reload the sessions instead of
        // calling a transcript load that has nothing to write into.
        if (state.agentsView.selectedAgentId != null &&
          document.querySelector("[data-repo-agent-transcript]")) {
          loadRepoAgentTranscript(state.selectedRepo, state.agentsView.selectedAgentId);
        } else {
          loadRepoAgents(state.selectedRepo);
        }
        return;
      }

      // Open an agent's detail page - live transcript + prompt (adhoc #259).
      // Toggle: clicking a selected agent returns to the list (issue #375).
      const agentOpenButton = event.target.closest("[data-repo-agent-open]");
      if (agentOpenButton && state.selectedRepo) {
        const agentId = agentOpenButton.dataset.repoAgentId || "";
        const composerMode = agentOpenButton.dataset.repoAgentComposerMode || "";
        if (String(state.agentsView.selectedAgentId ?? "") === String(agentId)) {
          closeRepoAgentDetail(state.selectedRepo);
        } else {
          openRepoAgentDetail(state.selectedRepo, agentId, composerMode);
        }
        return;
      }
      const agentBackButton = event.target.closest("[data-repo-agent-back]");
      if (agentBackButton && state.selectedRepo) {
        closeRepoAgentDetail(state.selectedRepo);
        return;
      }
  });

  document.addEventListener("change", (event) => {
    if (handleRepoForgeMirrorChange(event)) return;
    const discussionFilter = event.target.closest?.("[data-repo-discussion-filter]");
    if (discussionFilter) {
      setDiscussionFilter(discussionFilter.dataset.repoDiscussionFilter || "", discussionFilter.value);
      return;
    }
    const pullViewSelect = event.target.closest?.("[data-repo-pull-view-select]");
    if (pullViewSelect) {
      setPullView(pullViewSelect.value || "pulls");
      return;
    }
    const pullFilterSelect = event.target.closest?.("[data-repo-pull-filter]");
    if (pullFilterSelect) {
      setPullCollectionFilter(pullFilterSelect.dataset.repoPullFilter || "", pullFilterSelect.value);
      return;
    }
    const issueViewSelect = event.target.closest?.("[data-repo-issue-view-select]");
    if (issueViewSelect) {
      setIssueView(issueViewSelect.value || "issues");
      return;
    }
    const issueFilter = event.target.closest?.("[data-repo-issue-filter]");
    if (issueFilter) {
      setIssueCollectionFilter(
        issueFilter.dataset.repoIssueFilter || "",
        issueFilter.value,
      );
      return;
    }
    const toggle = event.target.closest?.("[data-repo-issue-task-toggle]");
    if (toggle) void handleWebIssueTaskToggle(toggle);
  });

  // A click anywhere outside an open sidebar picker closes it and applies the
  // choices, the way GitHub's menus behave. Capture phase, so it runs before
  // the delegated handler acts on whatever was clicked.
  document.addEventListener("click", (event) => {
    if (!$("[data-repo-issue-picker]")) return;
    if (event.target.closest?.("[data-repo-issue-picker], [data-repo-issue-picker-open]")) return;
    closeIssuePicker({ apply: true });
  }, true);

  bindMentionAutocomplete();

  // The qualifier colouring is painted under a transparent input, so it has to
  // follow the input's horizontal scroll as the caret moves.
  document.addEventListener("selectionchange", () => {
    const input = document.activeElement;
    if (input?.matches?.('[data-repo-filter-query="issues"], [data-repo-filter-query="pulls"], [data-repo-filter-query="discussions"]')) renderIssueQueryHighlight(input);
  });

  document.addEventListener("dragstart", handleIssueTaskDragStart);
  document.addEventListener("dragover", handleIssueTaskDragOver);
  document.addEventListener("drop", handleIssueTaskDrop);
  document.addEventListener("dragend", handleIssueTaskDragEnd);

  document.addEventListener("contextmenu", (event) => {
    const treePathButton = event.target.closest("[data-dashboard-tree-path]");
    const blobPathButton = event.target.closest("[data-dashboard-blob-path]");
    if (!treePathButton && !blobPathButton) {
      closeRepoTreeContextMenu();
      return;
    }
    if (!state.selectedRepo) return;
    event.preventDefault();
    event.stopPropagation();
    if (treePathButton) {
      openRepoTreeContextMenu(event, {
        path: treePathButton.dataset.dashboardTreePath || "",
        kind: "tree",
      });
      return;
    }
    openRepoTreeContextMenu(event, {
      path: blobPathButton?.dataset.dashboardBlobPath || "",
      kind: "blob",
    });
  });

  document.addEventListener("submit", async (event) => {
    if (handleRepoActionsSubmit(event)) return;
    if (await handleRepoForgeMirrorSubmit(event)) return;
    const logoSuggestionForm = event.target.closest(
      "[data-repo-logo-suggestion-form]",
    );
    if (logoSuggestionForm && state.selectedRepo) {
      event.preventDefault();
      const submit = logoSuggestionForm.querySelector('button[type="submit"]');
      if (submit) submit.disabled = true;
      setRepoLogoSuggestionStatus("Submitting…");
      try {
        await submitRepoLogoSuggestion(state.selectedRepo, logoSuggestionForm);
        setRepoLogoSuggestionStatus(
          "Suggestion submitted for owner or moderator review.",
          "good",
        );
      } catch (error) {
        const code = String(error?.message || "");
        setRepoLogoSuggestionStatus(
          code === "sign_in_required"
            ? "Sign in to suggest a logo."
            : code === "rights_required"
              ? "Confirm that you have rights to submit the image."
              : code === "logo_too_large"
                ? "Choose a PNG, JPEG, or WebP image up to 256 KB."
                : "The logo suggestion could not be submitted.",
          "bad",
        );
      } finally {
        if (submit) submit.disabled = false;
      }
      return;
    }
    const webhookForm = event.target.closest("[data-repo-webhook-form]");
    if (webhookForm && state.selectedRepo) {
      event.preventDefault();
      const submit = webhookForm.querySelector('button[type="submit"]');
      const url = String(webhookForm.querySelector("[data-repo-webhook-url]")?.value || "").trim();
      const description = String(webhookForm.querySelector("[data-repo-webhook-description]")?.value || "").trim();
      const events = Array.from(
        webhookForm.querySelectorAll("[data-repo-webhook-event]"),
      ).filter((input) => input.checked).map((input) => input.dataset.repoWebhookEvent);
      if (!events.length) {
        setRepoWebhookStatus("Choose at least one event.", "bad");
        return;
      }
      if (submit) submit.disabled = true;
      setRepoWebhookStatus("Adding…");
      try {
        const body = await repoWebhookRequest(state.selectedRepo, {
          action: "create", url, description, events,
        });
        webhookForm.reset();
        setRepoWebhookStatus("Webhook added.");
        // The secret exists only in this response; surface it before the list
        // reload repaints the card.
        showRepoWebhookSecret(body?.webhook?.secret || "");
        await loadRepoWebhooks(state.selectedRepo);
      } catch (error) {
        setRepoWebhookStatus(repoWebhookErrorText(error?.message), "bad");
      } finally {
        if (submit) submit.disabled = false;
      }
      return;
    }
    const settingsForm = event.target.closest("[data-repo-settings-form]");
    if (settingsForm && state.selectedRepo) {
      event.preventDefault();
      const submit = settingsForm.querySelector('button[type="submit"]');
      if (submit) submit.disabled = true;
      setRepoSettingsStatus("Saving…");
      try {
        const saved = await saveRepoSettingsFromWeb(state.selectedRepo, {
          fediverse: {
            federate: Boolean(settingsForm.querySelector("[data-repo-ap-federate]")?.checked),
            broadcastEvents: Boolean(settingsForm.querySelector("[data-repo-ap-broadcast]")?.checked),
            acceptComments: Boolean(settingsForm.querySelector("[data-repo-ap-comments]")?.checked),
          },
          features: Object.fromEntries(REPO_FEATURE_SWITCHES.map((key) => [
            key, Boolean(settingsForm.querySelector(`[data-repo-feature="${key}"]`)?.checked),
          ])),
        });
        applyRepoFeatures(saved?.features);
        setRepoSettingsStatus("Settings saved.", "good");
        loadRepoFediverse(state.selectedRepo);
        loadRepoDigestPreview(state.selectedRepo);
      } catch (error) {
        const code = String(error?.message || "");
        setRepoSettingsStatus(
          code === "not_authorized"
            ? "Only the repository owner or an organization owner or admin can change repository settings."
            : code === "account_required"
              ? "Sign in as the source node owner first."
              : "Could not save repository settings.",
          "bad",
        );
      } finally {
        if (submit) submit.disabled = false;
      }
      return;
    }

    const aboutForm = event.target.closest("[data-repo-about-form]");
    if (aboutForm && state.selectedRepo) {
      event.preventDefault();
      const input = aboutForm.querySelector("[data-repo-about-input]");
      const submit = aboutForm.querySelector('button[type="submit"]');
      const description = String(input?.value || "").trim();
      if (submit) submit.disabled = true;
      setRepoAboutStatus("Saving...");
      try {
        // Optional branding uploads ride along with the description: a chosen
        // file becomes a data-URL PNG, a checked "Remove" sends "" (clear),
        // and an untouched image is simply omitted (left unchanged).
        const media = {};
        const readAsDataUrl = (file) => new Promise((resolve, reject) => {
          const reader = new FileReader();
          reader.onload = () => resolve(String(reader.result || ""));
          reader.onerror = () => reject(new Error("image_read_failed"));
          reader.readAsDataURL(file);
        });
        const logoFile = aboutForm.querySelector("[data-repo-about-logo]")?.files?.[0];
        const bannerFile = aboutForm.querySelector("[data-repo-about-banner]")?.files?.[0];
        if (logoFile && logoFile.size > 256 * 1024) throw new Error("logo_too_large");
        if (bannerFile && bannerFile.size > 1024 * 1024) throw new Error("banner_too_large");
        if (aboutForm.querySelector("[data-repo-about-logo-clear]")?.checked) media.logoPng = "";
        else if (logoFile) media.logoPng = await readAsDataUrl(logoFile);
        if (aboutForm.querySelector("[data-repo-about-banner-clear]")?.checked) media.bannerPng = "";
        else if (bannerFile) media.bannerPng = await readAsDataUrl(bannerFile);
        const website = String(
          aboutForm.querySelector("[data-repo-about-website-input]")?.value || "").trim();
        media.website = website;
        const body = await saveRepoAboutFromWeb(state.selectedRepo, description, media);
        applyRepoAboutDescription(state.selectedRepo, body.description ?? description);
        applyRepoAboutWebsite(website);
        setRepoAboutStatus("Saved.", "good");
        setRepoAboutEditing(false);
        // Refresh the badge header + watch count so the new logo/banner (and
        // the ?v= cache-buster) show immediately.
        loadRepoFediverse(state.selectedRepo);
      } catch (error) {
        const code = String(error?.message || "");
        setRepoAboutStatus(
          code === "not_authorized" ? "Only the repository owner or an organization owner or admin can edit About."
            : code === "account_required" ? "Sign in as the source node owner first."
            : code === "logo_too_large" ? "Logo must be a PNG up to 256 KB."
            : code === "banner_too_large" ? "Banner must be a PNG up to 1 MB."
            : code === "image_too_large" ? "Image too large (logo ≤256 KB, banner ≤1 MB)."
            : code === "bad_image" ? "Images must be PNG files."
            : "Could not save About.",
          "bad");
      } finally {
        if (submit) submit.disabled = false;
      }
      return;
    }
    const issueForm = event.target.closest("[data-repo-issue-form]");
    if (issueForm && state.selectedRepo) {
      event.preventDefault();
      handleIssueComposeSubmit(state.selectedRepo, issueForm);
      return;
    }
    const issueImportForm = event.target.closest("[data-repo-issue-import-form]");
    if (issueImportForm && state.selectedRepo) {
      event.preventDefault();
      handleIssueImportSubmit(state.selectedRepo, issueImportForm);
      return;
    }
    const issueCommentForm = event.target.closest("[data-repo-issue-comment-form]");
    if (issueCommentForm && state.selectedRepo) {
      event.preventDefault();
      issueCommentForm._issueSubmitAction =
        event.submitter?.dataset.repoIssueCommentAction || "comment";
      handleIssueCommentSubmit(state.selectedRepo, issueCommentForm);
      return;
    }
    const issueTitleForm = event.target.closest("[data-repo-issue-title-form]");
    if (issueTitleForm) {
      event.preventDefault();
      void handleWebIssueTitleSubmit(issueTitleForm);
      return;
    }
    const issueDescriptionForm = event.target.closest(
      "[data-repo-issue-description-form]",
    );
    if (issueDescriptionForm) {
      event.preventDefault();
      void handleWebIssueDescriptionSubmit(issueDescriptionForm);
      return;
    }
    const issueMetadataForm = event.target.closest(
      "[data-repo-issue-metadata-form]",
    );
    if (issueMetadataForm) {
      event.preventDefault();
      void handleWebIssueMetadataSubmit(issueMetadataForm);
      return;
    }
    const discussionReplyForm = event.target.closest("[data-repo-discussion-reply-form]");
    if (discussionReplyForm && state.selectedRepo) {
      event.preventDefault();
      handleDiscussionReplySubmit(state.selectedRepo, discussionReplyForm);
      return;
    }
    const pullReviewForm = event.target.closest("[data-repo-pull-review-form]");
    if (pullReviewForm && state.selectedRepo) {
      event.preventDefault();
      const action = event.submitter?.dataset.repoPullReviewAction || "comment";
      handlePullReviewSubmit(state.selectedRepo, pullReviewForm, action);
      return;
    }
    const pullInlineCommentForm = event.target.closest("[data-repo-pull-inline-comment-form]");
    if (pullInlineCommentForm && state.selectedRepo) {
      event.preventDefault();
      const action = event.submitter?.dataset.repoPullInlineAction || "now";
      void submitPullInlineCommentForm(state.selectedRepo, pullInlineCommentForm, action);
      return;
    }
    const remoteForkForm = event.target.closest("[data-repo-remote-fork-form]");
    if (remoteForkForm && state.selectedRepo) {
      event.preventDefault();
      const input = remoteForkForm.querySelector("[data-repo-remote-fork-url]");
      void saveRepoRemoteFork(state.selectedRepo, "POST", String(input?.value || "").trim()).then((saved) => {
        if (saved && input) input.value = "";
      });
      return;
    }
    const pullNewForm = event.target.closest("[data-repo-pull-new-form]");
    if (pullNewForm && state.selectedRepo) {
      event.preventDefault();
      handlePullComposeSubmit(state.selectedRepo, pullNewForm);
      return;
    }
    const agentPromptForm = event.target.closest("[data-repo-agent-prompt-form]");
    if (agentPromptForm && state.selectedRepo) {
      event.preventDefault();
      handleRepoAgentPromptSubmit(state.selectedRepo, agentPromptForm);
      return;
    }
    const agentNewForm = event.target.closest("[data-repo-agent-new-form]");
    if (agentNewForm) {
      event.preventDefault();
      // The form lives in the header modal (adhoc #62): the target repo comes
      // from the modal's own repository picker, not the open repo page.
      handleRepoAgentNewSubmit(agentModalSelectedRepo(), agentNewForm);
    }
  });

  // The pull request explorer is re-rendered per keystroke, but only ever from
  // the changed-file list already in memory - no mirror read behind it.
  document.addEventListener("input", (event) => {
    if (event.target.closest("[data-repo-pull-file-filter]")) refreshRepoPullExplorer();
  });

  // GitHub-style range selection starts on a line-number gutter and extends
  // across matching line numbers while the pointer is held. The composer is
  // inserted beneath the last visual row in the selected range, inside the
  // diff block, so code context never disappears behind a modal.
  // A finger on the gutter is almost always the start of a scroll, so touch
  // and pen are left to the browser here (no preventDefault, no drag) and the
  // tap's click selects instead (tapPullDiffLine).
  let lastPullDiffLinePointerType = "mouse";
  document.addEventListener("pointerdown", (event) => {
    const button = event.target.closest("[data-repo-pull-diff-line]");
    if (!button || event.button !== 0) return;
    lastPullDiffLinePointerType = event.pointerType || "mouse";
    if (lastPullDiffLinePointerType !== "mouse") return;
    event.preventDefault();
    beginPullDiffSelection(button, event.pointerId);
  });

  document.addEventListener("pointerover", (event) => {
    const selection = state.repoPullDiffSelection;
    if (!selection || selection.pointerId === null) return;
    const button = event.target.closest("[data-repo-pull-diff-line]");
    if (button) extendPullDiffSelection(button);
  });

  document.addEventListener("pointermove", (event) => {
    const selection = state.repoPullDiffSelection;
    if (!selection || selection.pointerId !== event.pointerId) return;
    const button = document.elementFromPoint(event.clientX, event.clientY)
      ?.closest("[data-repo-pull-diff-line]");
    if (button) extendPullDiffSelection(button);
  });

  document.addEventListener("pointerup", (event) => {
    const selection = state.repoPullDiffSelection;
    if (!selection || selection.pointerId !== event.pointerId) return;
    selection.pointerId = null;
    finishPullDiffSelection();
  });

  document.addEventListener("pointercancel", (event) => {
    const selection = state.repoPullDiffSelection;
    if (!selection || selection.pointerId !== event.pointerId) return;
    clearPullDiffSelection();
  });

  // Keep the explorer's highlight on whichever file the diff pane is showing.
  // The dashboard shell scrolls an inner <section>, not the window, so this
  // listens in the capture phase to catch scroll from whichever element owns it.
  // One passive listener, coalesced into a frame, and it exits immediately when
  // the Files tab is not the visible panel.
  let repoPullScrollFrame = 0;
  document.addEventListener("scroll", () => {
    if (repoPullScrollFrame) return;
    repoPullScrollFrame = window.requestAnimationFrame(() => {
      repoPullScrollFrame = 0;
      syncRepoPullActiveFileFromScroll();
    });
  }, { capture: true, passive: true });

  // The reader taking the scroll back ends an explorer jump's settling.
  ["touchstart", "wheel", "keydown"].forEach((type) => {
    document.addEventListener(type, cancelRepoPullDiffJump, { capture: true, passive: true });
  });

  // Rotating a phone (or resizing a window) across the split breakpoint
  // re-lays the diff in the layout that fits.
  window.matchMedia?.(PULL_DIFF_NARROW_QUERY)?.addEventListener?.("change", () => {
    const list = document.querySelector("[data-repo-pull-diff-list]");
    if (list && list.dataset.diffMode !== effectivePullDiffMode(pullLayoutPrefs().diff)) {
      refreshRepoPullDiff();
    }
  });

  $("#repoSearch")?.addEventListener("input", () => {
    const startedAt = interfaceNow();
    state.page = 1;
    applyRepositoryFilter();
    recordInterfaceMetric("search", startedAt);
  });
  $("[data-global-search]")?.addEventListener("focus", () => {
    setGlobalSearchOpen(true);
  });
  $("[data-global-search]")?.addEventListener("input", () => {
    const startedAt = interfaceNow();
    state.globalSearch.open = true;
    state.globalSearch.selectedIndex = 0;
    // The dropdown searches the whole catalog; the page filter narrows what is
    // already on screen. Both run per keystroke (adhoc #37) — the page filter
    // only re-renders in-memory lists, so there is no fetch behind it.
    applyGlobalSearchPageFilter();
    renderGlobalSearchResults();
    recordInterfaceMetric("search", startedAt);
  });
  $("[data-global-search]")?.addEventListener("keydown", (event) => {
    if (event.key === "ArrowDown") {
      event.preventDefault();
      moveGlobalSearchSelection(1);
      return;
    }
    if (event.key === "ArrowUp") {
      event.preventDefault();
      moveGlobalSearchSelection(-1);
      return;
    }
    if (event.key === "Enter") {
      event.preventDefault();
      selectGlobalSearchResult();
      return;
    }
    if (event.key === "Escape") {
      event.preventDefault();
      closeGlobalSearch();
    }
  });
  $("[data-home-repo-search]")?.addEventListener("input", () => {
    renderHomeRepositories();
  });
  $("[data-home-repos-show-more]")?.addEventListener("click", () => {
    state.homeRepositoriesExpanded = !state.homeRepositoriesExpanded;
    renderHomeRepositories();
  });
  $("[data-profile-repo-search]")?.addEventListener("input", () => {
    renderProfileRepositories();
  });
  $("[data-repo-prev]")?.addEventListener("click", () => {
    state.page -= 1;
    updateRepositoryPagination();
  });
  $("[data-repo-next]")?.addEventListener("click", () => {
    state.page += 1;
    updateRepositoryPagination();
  });

  // New-repository modal (adhoc #30): open from the Repos header, collect the
  // create-and-mirror details, then hand off to the desktop node (see
  // handleNewRepoSubmit — the signed publish + git mirror are desktop-only).
  $("[data-new-repo-close]")?.addEventListener("click", () => setNewRepoModalOpen(false));
  $("[data-new-repo-backdrop]")?.addEventListener("click", () => setNewRepoModalOpen(false));
  $("[data-new-repo-cancel]")?.addEventListener("click", () => {
    if ($("[data-new-repo-page]")) {
      location.assign("/dashboard/repos");
      return;
    }
    setNewRepoModalOpen(false);
  });
  $$("[data-new-repo-source]").forEach((btn) => {
    btn.addEventListener("click", () => setNewRepoSource(btn.dataset.newRepoSource));
  });
  $("[data-new-repo-form]")?.addEventListener("submit", (event) => {
    event.preventDefault();
    handleNewRepoSubmit();
  });
  // The push block for a ForkMesh-hosted repository (issue #676): the whole
  // point is getting these commands into a terminal, so copying them is one
  // click rather than a careful selection across a wrapped <pre>.
  $("[data-new-repo-push-copy]")?.addEventListener("click", async () => {
    const label = $("[data-new-repo-push-copy-label]");
    const copied = await copyTextToClipboard(
      $("[data-new-repo-push-commands]")?.textContent || "");
    if (label) label.textContent = copied ? "Copied" : "Copy failed";
  });
  $("[data-external-repo-list]")?.addEventListener("click", (event) => {
    const reportButton = event.target.closest("[data-repository-migration-report]");
    if (reportButton) {
      const job = state.repositoryMigrations.find(
        (candidate) => String(candidate?.id || "") === String(reportButton.dataset.repositoryMigrationReport || ""));
      downloadRepositoryMigrationReport(job, reportButton);
      return;
    }
    const retryButton = event.target.closest("[data-repository-migration-retry]");
    if (retryButton) {
      const job = state.repositoryMigrations.find(
        (candidate) => String(candidate?.id || "") === String(retryButton.dataset.repositoryMigrationRetry || ""));
      retryRepositoryMigration(job, retryButton);
      return;
    }
    const resume = event.target.closest("[data-repository-migration-resume]");
    if (resume) {
      const job = state.repositoryMigrations.find(
        (candidate) => String(candidate?.id || "") === String(resume.dataset.repositoryMigrationResume || ""));
      continueRepositoryMigration(job, resume);
      return;
    }
    const button = event.target.closest("[data-external-mirror-volunteer]");
    if (!button) return;
    volunteerForExternalMirror(
      button.dataset.externalMirrorVolunteer || "", button);
  });
  $("[data-external-repo-list]")?.addEventListener("change", (event) => {
    const checkbox = event.target.closest("[data-external-repo-select]");
    if (!checkbox) return;
    const id = String(checkbox.dataset.externalRepoSelect || "");
    if (checkbox.checked) state.externalRepositorySelection.add(id);
    else state.externalRepositorySelection.delete(id);
    syncExternalRepositoryActions();
  });
  $("[data-external-repo-select-all]")?.addEventListener("change", (event) => {
    toggleAllExternalRepositories(Boolean(event.currentTarget.checked));
  });
  $("[data-external-repo-delete-selected]")?.addEventListener("click", () => {
    deleteSelectedExternalRepositories();
  });

  // [data-profile-settings-button] is a real link to /dashboard/settings now,
  // and [data-settings-section-link] clicks are handled by the delegated
  // document click handler above (with push: true for URL reflection).
  $("[data-profile-modal-close]")?.addEventListener("click", () => setProfileModalOpen(false));
  $("[data-profile-modal-backdrop]")?.addEventListener("click", () => setProfileModalOpen(false));
  // Profile saves and verification sends are handled by the delegated document
  // click handler above. Direct listeners would submit the same action twice.
  $("[data-profile-rename-input]")?.addEventListener("input", () => {
    window.clearTimeout(state.nodeNameAvailability.timer);
    state.nodeNameAvailability.timer = window.setTimeout(checkNodeNameAvailability, 250);
  });
  $("[data-profile-rename-password]")?.addEventListener("input", updateRenameButton);
  $("[data-profile-delete-confirm]")?.addEventListener("input", () => {
    const button = $("[data-profile-delete-account]");
    if (!button) return;
    button.disabled = ($("[data-profile-delete-confirm]")?.value || "").trim() !== "DELETE";
    button.classList.toggle("opacity-40", button.disabled);
  });

  $("[data-ssh-key-form]")?.addEventListener("submit", (event) => {
    event.preventDefault();
    addSshKey();
  });

  document.addEventListener("input", (event) => {
    if (event.target?.matches?.("[data-repo-issue-picker-search]")) {
      setIssuePickerQuery(event.target.value || "");
      return;
    }
    // The lifecycle button reads "Close issue" on an empty box and
    // "Close with comment" once there is a draft.
    if (event.target?.matches?.("[data-repo-issue-comment-body]")) {
      const label = event.target.closest("[data-repo-issue-comment-form]")
        ?.querySelector("[data-repo-issue-comment-close-label]");
      if (label) {
        label.textContent = event.target.value.trim()
          ? label.dataset.draftLabel : label.dataset.emptyLabel;
      }
      return;
    }
    if (event.target?.matches?.('[data-repo-filter-query="issues"]') && state.selectedRepo) {
      // The box holds qualifiers and free text together; parse both back into
      // the filters, then repaint the colouring under the caret.
      applyIssueQueryText(event.target.value || "");
      renderIssueQueryHighlight(event.target);
      return;
    }
    if (event.target?.matches?.('[data-repo-filter-query="discussions"]') && state.selectedRepo) {
      applyDiscussionQueryText(event.target.value || "");
      renderIssueQueryHighlight(event.target);
      return;
    }
    if (event.target?.matches?.('[data-repo-filter-query="pulls"]') && state.selectedRepo) {
      applyPullQueryText(event.target.value || "");
      renderIssueQueryHighlight(event.target);
      return;
    }
    if (event.target?.matches?.("[data-repo-branches-search]") && state.selectedRepo) {
      setRepoBranchesQuery(state.selectedRepo, event.target.value || "");
      renderRepoBranchesList(state.selectedRepo);
      return;
    }
    if (event.target?.matches?.("[data-repo-branch-search]") && state.selectedRepo) {
      setRepoBranchQuery(state.selectedRepo, event.target.value || "");
      updateRepoBranchControls(state.selectedRepo, event.target.closest("[data-repo-branch-control]"));
      return;
    }
    if (event.target?.matches?.("[data-repo-file-finder-input]")) {
      state.repoFileFinder.selectedIndex = 0;
      renderRepoFileFinderResults(event.target.value || "");
    }
  });

  document.addEventListener("keydown", (event) => {
    if (handleIssueTaskReorderKey(event)) return;
    // A sidebar picker closes (and applies) on Escape; Enter in its filter
    // picks the first entry.
    if ($("[data-repo-issue-picker]")) {
      if (event.key === "Escape") {
        event.preventDefault();
        closeIssuePicker({ apply: true });
        return;
      }
      if (event.key === "Enter" && event.target?.matches?.("[data-repo-issue-picker-search]")) {
        event.preventDefault();
        $("[data-repo-issue-picker-option]")?.click();
        return;
      }
    }
    // Enter in the issue search tidies the box into canonical qualifiers.
    if (event.key === "Enter" && event.target?.matches?.('[data-repo-filter-query="issues"]')) {
      event.preventDefault();
      syncIssueQueryBox({ force: true });
      return;
    }
    if (event.key === "Enter" && event.target?.matches?.('[data-repo-filter-query="pulls"]')) {
      event.preventDefault();
      syncPullQueryBox({ force: true });
      return;
    }
    const typingTarget = event.target?.matches?.("input, textarea, select, [contenteditable='true']");
    // Ctrl/Cmd+Enter submits the form being typed in (issue #687): new issue,
    // new pull request, comments, reviews. It goes through the form's primary
    // submit button, so validation and the submit handler (and its
    // event.submitter action) run as for a click, and a disabled (in-flight)
    // button makes it a no-op instead of a second submission. Multi-action
    // forms put a secondary action ("Close with comment", "Request changes")
    // first, so the HTML default button is not used; a form with several
    // actions and no primary one is left alone. Fields with their own Enter
    // handling mark the event.
    if (typingTarget && event.key === "Enter" && (event.metaKey || event.ctrlKey)
        && !event.isComposing && !event.repeat && !event.defaultPrevented
        && event.target.form) {
      const submitters = [...event.target.form.querySelectorAll(
        "button[type='submit'], input[type='submit']")];
      const submitter = submitters.find((button) => button.matches(".bg-cta, .fm-btn-cta"))
        || (submitters.length === 1 ? submitters[0] : null);
      if (submitter) {
        event.preventDefault();
        if (!submitter.disabled) event.target.form.requestSubmit(submitter);
        return;
      }
    }
    if (!typingTarget && event.key === "/") {
      if (focusGlobalSearch()) event.preventDefault();
      return;
    }
	    if (fileFinderOpen() && event.key === "Tab") {
	      const dialog = $("[data-repo-file-finder-dialog]");
	      const focusable = dialog ? [...dialog.querySelectorAll("button:not([disabled]):not([tabindex='-1']), input:not([disabled]):not([tabindex='-1']), select:not([disabled]):not([tabindex='-1']), textarea:not([disabled]):not([tabindex='-1']), [href]:not([tabindex='-1']), [tabindex]:not([tabindex='-1'])")].filter((element) => !element.hidden) : [];
	      if (focusable.length) {
	        event.preventDefault();
	        const current = focusable.indexOf(document.activeElement);
	        const next = (current + (event.shiftKey ? -1 : 1) + focusable.length) % focusable.length;
	        focusable[next].focus();
	      }
	      return;
	    }
	    if (event.key === "Escape") {
	      setProfileModalOpen(false);
	      setNotificationDropdownOpen(false);
	      setNotificationModalOpen(false);
	      setAgentModalOpen(false);
	      setNewRepoModalOpen(false);
	      closeRepoBranchMenus();
	      closeRepoTreeContextMenu();
	      closeRepoFileFinder();
	      closeGlobalSearch();
	      closeMobileDrawers();
	      return;
	    }
    const openCard = event.target?.closest?.("[data-dashboard-open-repo][role='link']");
    if (!typingTarget && openCard && (event.key === "Enter" || event.key === " ")) {
      event.preventDefault();
      openRepoPage(openCard.dataset.dashboardOpenRepo);
      return;
    }
    if (fileFinderOpen()) {
      if (event.key === "ArrowDown") {
        event.preventDefault();
        moveRepoFileFinderSelection(1);
        return;
      }
      if (event.key === "ArrowUp") {
        event.preventDefault();
        moveRepoFileFinderSelection(-1);
        return;
      }
      if (event.key === "Enter") {
        event.preventDefault();
        selectRepoFileFinderResult();
        return;
      }
    }
    if (!typingTarget && event.key.toLowerCase() === "t" && state.selectedRepo) {
      event.preventDefault();
      openRepoFileFinder(event.target);
    }
  });

  // History handling is page-scoped now: only the repo page (tabs/tree/blob)
  // and the settings page (sub-tabs) push same-document states. Back/Forward
  // across pages is native navigation between real documents.
  function initPageHistory() {
    const page = currentPage();
    if (page === "settings") {
      window.addEventListener("popstate", () => {
        setSettingsSection(settingsSectionFromPath(), { scroll: false });
      });
      return;
    }
    if (page !== "repo") return;
    window.addEventListener("popstate", () => {
      const requested = requestedRepoKey();
      const repo = requested ? findRepository(requested) : null;
      if (!repo) {
        // The entry points outside this repo document — a real navigation.
        location.reload();
        return;
      }
      if (state.selectedRepo && repoKey(state.selectedRepo) === repoKey(repo)) {
        // Same repo; restore the path/tab from the URL instead of tearing down
        // and rebuilding the whole detail view.
        const parts = repoRouteParts();
        const kind = parts[2];
        const path = parts.length > 3 ? parts.slice(3).map(decodeURIComponent).join("/") : "";
        const compareRoute = kind === "compare" ? parseRepoCompareRoute(parts) : null;
        if (compareRoute) {
          // Back/Forward onto a comparison: the diff viewer, not the list.
          openRepoCompare(repo, compareRoute.base, compareRoute.head, { navigate: false });
        } else if (repoTabRoutesFor(repo).includes(kind)) {
          // Feature tab (issues, pulls, etc.): restore without re-loading
          // records since they cache in state.
          setRepoTab(kind);
          if (kind === "security") void loadRepoSecurity(repo);
          if (kind === "commits") {
            const commitHash = parts[3] ? safeDecodeURIComponent(parts[3]) : "";
            if (/^[0-9a-f]{7,64}$/i.test(commitHash)) loadRepoCommitDetail(repo, commitHash);
            else if (state.repoCommitDetail) {
              state.repoCommitDetail = null;
              loadRepoCommits(repo);
            }
          }
          // Step Back/Forward between a record detail (/pulls/4), one of its
          // sections (/pulls/4/files) and the list.
          if (["pulls", "discussions", "issues"].includes(kind)) {
            const recordNumber = parts[3] ? safeDecodeURIComponent(parts[3]) : "";
            if (kind === "pulls" && recordNumber === "send") {
              void openPullBatch(repo, { navigate: false });
            } else if (/^\d+$/.test(recordNumber)) {
              const section = kind === "pulls"
                ? normalizeRepoRecordSection(parts[4])
                : "conversation";
              const sameRecord = state.repoRecordDetail?.kind === kind
                && String(state.repoRecordDetail.number) === recordNumber;
              state.repoRecordSection = section;
              // Stepping between two sections of the record already on screen
              // is a tab switch, not a reason to refetch it from the mirror.
              if (sameRecord) setRepoRecordSection(section);
              else loadRepoRecordDetail(repo, kind, recordNumber);
            } else if (state.repoRecordDetail?.kind === kind
                || (kind === "pulls" && (state.repoCompare || document.querySelector("[data-pull-batch], [data-pull-batch-message]")))) {
              if (kind === "pulls") stopRepoPullActivityRefresh();
              state.repoRecordDetail = null;
              // Stepping back from a comparison lands on the list it left.
              state.repoCompare = null;
              // Issues re-render through their filtered list (loadRepoCollection
              // would leak closed issues into the default Open view); fetch it
              // if a deep-link landing never populated the list.
              if (kind === "issues") {
                if (state.issuesView.items.length) renderRepoIssues();
                else loadRepoIssues(repo);
              } else {
                loadRepoCollection(repo, kind, `[data-repo-${kind}]`);
              }
            }
          }
        } else if (kind === "blob" && path) {
          loadRepositoryBlob(repo, path);
        } else {
          loadRepositoryTree(repo, kind === "tree" ? path : "");
        }
        return;
      }
      renderRepoDetail(repo);
    });
  }

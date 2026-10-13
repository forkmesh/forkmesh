(() => {
  const state = {
    repositories: [],
    externalRepositories: [],
    repositoryMigrations: [],
    externalRepositoriesLoading: false,
    externalRepositorySelection: new Set(),
    // repository id -> node name volunteered from this page (issue #680), so
    // the card can confirm the request instead of re-offering the button.
    externalMirrorVolunteers: new Map(),
    filteredRepositories: [],
    filteredGroups: [],
    repositoriesLoading: true,
    page: 1,
    pageSize: 100,
    selectedRepo: null,
    // The webhook event vocabulary the relay published on the last Settings
    // load, so the "add webhook" check-boxes are the server's list rather than
    // a copy that can drift out of date in this file.
    repoWebhookEvents: [],
    repoForgeMirror: null,
    // Per-account push channel (public/account-events.js). Held for the life of
    // the page: it carries session revocation and unread-count refreshes that
    // used to be timers.
    accountEvents: null,
    // The streamed provider sign-in session on an owned node, while its dialog
    // is open (public/agent-terminal.js). It holds its own short-lived socket
    // rather than sharing accountEvents, so closing the dialog ends the node's
    // pty instead of leaving one attached to the page's lifetime.
    agentLoginTerminal: null,
    fetchJsonInflight: {},
    fetchJsonCache: {},
    // Invalidates canonical profile reads that started before a profile write.
    // Without this guard, a slow boot-time GET can paint its old snapshot over
    // the successful POST response and put the old values back in storage.
    profileMutationVersion: 0,
    selectedBranches: {},
    repoBranches: {},
    repoBranchQueries: {},
    // The Branches page's own filter box. Kept apart from repoBranchQueries
    // (the toolbar dropdown's filter) so typing in one does not silently
    // narrow the other.
    repoBranchesQueries: {},
    // Branch management state for the Branches page (adhoc #2098): per repo,
    // the queued operations the owner's node has not taken yet, the merge
    // trials that decided whether a diverged branch still merges cleanly,
    // whether this viewer may manage branches at all, and the one operation
    // currently in flight.
    repoBranchOps: {},
    repoBranchTrials: {},
    repoBranchManage: {},
    repoBranchBusy: {},
    repoBranchOpsWatch: {},
    repoBranchRunning: {},
    repoBranchOutcome: {},
    repoBranchResume: {},
    repoBranchHeld: {},
    repoPullMetadataCommits: {},
    repoPullMetadataInflight: {},
    repoCollectionPages: {},
    session: null,
    // Public-profile mode (/@name): the FOREIGN account whose profile the
    // page is showing, or null when the profile pages show the session user.
    publicProfile: null,
    repoFileFinder: {
      repoKey: "",
      files: [],
      indexed: false,
      indexing: false,
      partial: false,
      error: "",
      selectedIndex: 0,
    },
    nodeNameAvailability: {
      candidate: "",
      available: false,
      checking: false,
      seq: 0,
    },
    notifications: [],
    notificationUnread: 0,
    selectedNotificationId: "",
    issuesView: {
      filter: "open",
      items: [],
      query: "",
      filters: {
        author: "", label: "", milestone: "", assignee: "",
        type: "", sort: "recently_updated",
      },
    },
    pullsView: {
      filter: "open",
      query: "",
    },
    // Projects tab (issue #384): projects link issues + a milestone and carry
    // start/end dates; "gantt" is the default sub-view, "list" the fallback.
    projectsView: { filter: "open", mode: "gantt", items: [] },
    claimNode: { pendingNodeId: "" },
    linkGrant: null,
    repoMirrors: [],
    // Mirrors tab filter: hide nodes that are not serving this repository
    // right now. Off by default so the tab opens on the full published mesh.
    repoMirrorsOnlineOnly: false,
    repoSourceOfTruth: null,
    repoLatestCommit: null,
    repoServedBy: null,
    // Owner-only "Agents" tab (adhoc #225): owner verification by node account,
    // no password required. selectedAgentId (adhoc #259) is the id of the agent
    // whose detail page - live transcript + prompt - is currently open, or null
    // for the session list.
    // canViewSessions carries the last fetch's verdict so the list can redraw
    // itself under the header search filter without re-asking (adhoc #62).
    agentsView: { agents: [], selectedAgentId: null, selectedAgentComposerMode: "", canViewSessions: false, canDeleteSessions: false, canQueueAgent: false, ownerScope: false, accessReason: "", notice: "", startPayload: null, desktopNotice: "", desktopNoticeTone: "" },
    homeAgentSessions: null,
    // Home right-rail "Latest from the blog" (adhoc #381): the newest posts
    // parsed from /blog/rss.xml. null until the feed read resolves so the card
    // can tell "loading" apart from "feed unavailable".
    homeBlogPosts: null,
    repoRemoteForks: { key: "", forks: [], canManage: false, loaded: false },
    // Organization aliases are not duplicate catalog publications: load the
    // signed-in viewer's linked aliases separately so Top repositories can
    // show the stable organization path with a clear owner label.
    homeOrganizationRepositories: [],
    // Whether the home left rail's "Top repositories" list is expanded past
    // its short head. The "Show more" control that flips this only appears for
    // accounts with more repositories than the rail hides.
    homeRepositoriesExpanded: false,
    homeRepositoryViews: {},
    homeRepositoryViewsSignature: "",
    recordedRepositoryViews: new Set(),
    repoCommitDetail: null,
    repoRecordDetail: null,
    // The branch comparison on the Pull requests tab (/owner/repo/compare/
    // base...head), or null while the tab shows its list, a record, or the
    // new-pull form. Held so a slow compare read that lands after the reader
    // moved on cannot paint over what replaced it.
    repoCompare: null,
    // Newly accepted issues/PRs already have a relay-reserved durable number
    // before an online mirror finishes its git commit. Keep their signed
    // projection here so creation can redirect to the real numbered route.
    repoOptimisticRecords: {},
    // Which section of an open pull detail is showing (conversation | commits |
    // files | badge). Held in state, not just in the DOM, so the URL stays in
    // step with the visible tab and a re-render (a posted review, a refreshed
    // timeline) keeps the reader on the section they linked to.
    repoRecordSection: "conversation",
    // Inline pull-review comments can be queued before they are signed and
    // submitted. Keep them outside the rendered diff so changing layout or
    // filtering the file tree never throws a review batch away.
    repoPullReviewDrafts: [],
    repoPullInlineOpenThreads: new Set(),
    // parseDiffFiles() of the pull patch on screen, memoised for thread placement.
    repoPullDiffIndex: null,
    repoPullDiffSelection: null,
    // The explorer-driven scroll to a file while it is still settling
    // (scrollRepoPullDiffBlockIntoView); the scroll-spy stands down meanwhile.
    repoPullDiffJump: null,
    // A pull page can have a running organization agent attached to its head
    // branch. Keep the one short-lived timer here so navigating to another
    // record always supersedes the previous live poll.
    repoPullActivityRefreshTimer: 0,
    // Repository Actions tab (06-repo-actions.js); built on first use.
    repoActionsView: null,
    repoRecordConvergeTimer: 0,
    repoRecordConvergeAttempt: 0,
    // Last rendered pull/discussion list per kind, and the last rendered code
    // tree listing. Both are held so the header search box can re-filter what
    // is already on screen (adhoc #37) without a second mirror read.
    repoCollectionItems: {},
    repoTreeView: null,
    profileContributions: {
      range: null,
      data: null,
      loading: false,
      error: "",
      requestKey: "",
      selectedDay: "",
      cache: {},
    },
    settingsView: {
      section: "public-profile",
    },
    // /dashboard/tasks (adhoc #24): the organization-private task catalog, read
    // whole and then filtered and paged in the browser. `actor` is the relay's
    // name for the signed-in member, used by the "Assigned to me" filter.
    tasksView: {
      items: [],
      filter: "open",
      assignee: "all",
      query: "",
      page: 1,
      loading: true,
      error: "",
      actor: "",
      canManage: false,
      members: [],
      departments: [],
      teams: [],
      selectedId: "",
      busyId: "",
      serverNow: 0,
      trash: [],
      trashLoaded: false,
      trashLoading: false,
    },
    calendarView: {
      events: [], calendars: [], selected: new Set(["forkmesh:commits", "forkmesh:merges"]), mode: "month",
      anchor: new Date(), loading: true, error: "", actor: "", query: "",
      editingId: "", repositoryEvents: [], repositoryLoading: false,
      repositoryError: "", repositoryRange: "",
    },
    notesView: {
      items: [], selected: null, dirty: false, loading: true,
      error: "", socket: null, clientId: "", draftTimer: 0,
    },
    // /referrals: the public referral top 10 plus the inbound referring-site
    // board. `board` is the shared /api/referrals/leaderboard rows, so the
    // viewer's own counters are read out of it rather than fetched per-account.
    challengesView: {
      items: [],
      options: null,
      today: "",
      loading: true,
      error: "",
      notice: "",
      noticeTone: "good",
      invite: null,
      inviteId: "",
      inviteError: "",
      customizing: false,
      busy: false,
    },
    workflowsView: {
      catalog: null,
      workflows: [],
      credentials: [],
      current: null,
      dirty: false,
      selected: "",
      selectedEdge: -1,
      run: null,
      runs: [],
      runsOpen: false,
      loading: true,
      busy: false,
      notice: "",
      noticeTone: "good",
      zoom: 1,
      panX: 80,
      panY: 80,
      paletteQuery: "",
      paletteCategory: "",
      paletteAfter: null,
      credentialIntegration: "",
      lastField: null,
      mode: "automations",
      templates: [],
      repo: null,
      repoFiles: null,
      repoOptions: [],
      repoLoading: false,
      listOpen: false,
      sheetOpen: false,
      menuOpen: false,
      plugins: null,
      pluginOff: false,
      packageFiles: [],
    },
    bountiesView: {
      items: [],
      loading: true,
      error: "",
      access: "",
      filter: "open",
      busy: false,
      notice: "",
      noticeTone: "good",
    },
    linksView: {
      items: [],
      loading: true,
      error: "",
      busy: false,
      notice: "",
      noticeTone: "good",
    },
    referralsView: {
      board: [],
      loading: true,
      error: "",
      sites: [],
      sitesTotals: { sites: 0, visits: 0 },
      sitesLoading: true,
      sitesError: "",
    },
    // /dashboard/ads: ForkMesh Ads. `policy` is the public /api/ads/policy
    // document (limits, categories, prohibited rules, membership levels);
    // `mine` is the signed-in owner's /api/ads/mine answer. `editingId` is the
    // ad loaded into the form ("" = a new ad). The reviewer half only fills
    // when `mine.isReviewer` is true; the server enforces that either way.
    adsView: {
      policy: null,
      policyError: "",
      mine: null,
      loading: true,
      error: "",
      editingId: "",
      notice: "",
      noticeTone: "good",
      review: {
        status: "pending",
        counts: {},
        ads: [],
        prohibited: [],
        loading: false,
        error: "",
        notice: "",
        details: {},
      },
    },
    // /hackathon/me: one participant's own standing. A single GET fills the
    // whole page -- points, rank, missions, squad, onboarding and the share
    // kit -- so nothing on screen can disagree with anything else on it.
    hackathonView: {
      notice: "",
      data: null,
      loading: true,
      error: "",
    },
    // /microjobs: the public job catalog and, when signed in, this account's
    // own claims. One GET fills both, so the page never has to reconcile a
    // catalog and a claim list that were fetched at different moments.
    microjobsView: {
      jobs: [],
      claims: [],
      policy: null,
      account: "",
      payoutWallet: "",
      referralLink: "",
      earnedSol: "0",
      openJob: "",
      busy: false,
      loading: true,
      error: "",
      // Per-job form values, so re-rendering the card (busy state, then the
      // verdict) never throws away the report somebody just typed.
      drafts: {},
      notice: { job: "", text: "", ok: false },
    },
    globalSearch: {
      open: false,
      selectedIndex: 0,
      results: [],
      // The header search box also filters the page you are on, live, on top of
      // whatever that page's own filter box holds (adhoc #37). Kept here rather
      // than read off the input so a re-render after navigation still sees it.
      pageQuery: "",
    },
  };

  const $ = (selector) => document.querySelector(selector);
  const $$ = (selector) => Array.from(document.querySelectorAll(selector));

  // Pages contributed by market plugin packages (marketplace/packages/*/
  // content/web). Each plugin fragment registers its own state slice and boot
  // initializer here; core dashboard code never references a specific plugin.
  const pluginPages = {};
  const MAX_REPO_FILE_FINDER_RESULTS = 500;
  const MAX_REPO_FILE_FINDER_SECONDS = 6;
  const REPO_COLLECTION_PAGE_SIZE = 25;
  const DASHBOARD_THEME_KEY = "forkmesh.dashboard.theme";
  const AGENT_RECENT_REPOSITORIES_STORAGE_PREFIX =
    "forkmesh.agent.recentRepositories.v1:";
  const AGENT_RECENT_REPOSITORIES_LIMIT = 20;

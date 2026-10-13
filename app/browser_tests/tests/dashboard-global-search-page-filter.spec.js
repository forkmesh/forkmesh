// The header search box filters the page you are on, live, on top of the
// repository dropdown it already showed (adhoc #37). Source-text tests pin the
// wiring; this drives the real page to prove a keystroke actually narrows the
// rendered list and that clearing the box puts every row back.
const { test, expect } = require("@playwright/test");
const path = require("node:path");

const REPOS_PAGE_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "public",
  "dashboard",
  "repos",
  "index.html",
);

const REPO_PAGE_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "public",
  "dashboard",
  "repo.html",
);

const REPOSITORIES = [
  {
    owner: "alpha",
    name: "engine",
    description: "Physics core",
    commit: "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678",
  },
  { owner: "beta", name: "atlas", description: "Map tiles" },
  { owner: "gamma", name: "beacon", description: "Signal relay" },
].map((repo, index) => ({
  ...repo,
  visibility: "public",
  source: "remote-clone",
  cloneUrl: `https://forkmesh.test/${repo.owner}/${repo.name}`,
  rootCommit: `root-${index}`,
  stateHash: `state-${index}`,
  updatedAt: String(index + 1),
}));

async function stubDashboard(page) {
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({
      contentType: "text/javascript",
      body: "window.tailwind = {};",
    }),
  );
  await page.route(
    "https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js",
    (route) =>
      route.fulfill({
        contentType: "text/javascript",
        body: "window.lucide = { createIcons() {} };",
      }),
  );
  await page.route("**/dashboard/repos", (route) =>
    route.fulfill({
      path: REPOS_PAGE_PATH,
      contentType: "text/html; charset=utf-8",
    }),
  );
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    const body = url.pathname === "/api/repositories"
      ? { ok: true, repositories: REPOSITORIES }
      : { ok: true };
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
}

test("header search filters the repositories on the page as you type", async ({
  page,
}) => {
  await stubDashboard(page);
  await page.goto("/dashboard/repos");

  const cards = page.locator("#repoList .repo-card");
  await expect(cards).toHaveCount(REPOSITORIES.length);

  const search = page.locator("[data-global-search]");
  await search.fill("atlas");

  // The page itself narrows - not just the dropdown.
  await expect(cards).toHaveCount(1);
  await expect(cards.first()).toContainText("atlas");
  await expect(page.locator("[data-repo-summary]")).toContainText("of 1");

  // ...and the dropdown it always showed is still there, with the footer
  // saying the page behind it was filtered too.
  const results = page.locator("[data-global-search-result]");
  await expect(results).toHaveCount(1);
  await expect(page.locator("[data-global-search-page-filter]")).toContainText(
    "1 match",
  );

  // A term that matches the owner rather than the repo name still filters,
  // since the cards are labelled with the owner.
  await search.fill("gamma");
  await expect(cards).toHaveCount(1);
  await expect(cards.first()).toContainText("beacon");

  // A short commit-hash prefix finds the repository that advertises that
  // commit, just like names and descriptions do.
  await search.fill("a1b2c3d");
  await expect(cards).toHaveCount(1);
  await expect(cards.first()).toContainText("engine");
  await expect(results).toHaveCount(1);

  // Clearing the box restores every row and retires the footer.
  await search.fill("");
  await expect(cards).toHaveCount(REPOSITORIES.length);
  await expect(page.locator("[data-global-search-page-filter]")).toBeHidden();
});

test("header search filters the repo page's file listing in place", async ({
  page,
}) => {
  const treeRequests = [];
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({
      contentType: "text/javascript",
      body: "window.tailwind = {};",
    }),
  );
  await page.route(
    "https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js",
    (route) =>
      route.fulfill({
        contentType: "text/javascript",
        body: "window.lucide = { createIcons() {} };",
      }),
  );
  await page.route("**/alpha/engine", (route) =>
    route.fulfill({
      path: REPO_PAGE_PATH,
      contentType: "text/html; charset=utf-8",
    }),
  );
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = { ok: true, repositories: REPOSITORIES };
    } else if (url.pathname === "/api/repo/alpha/engine/tree") {
      treeRequests.push(url.pathname);
      body = {
        ok: true,
        commit: "0123456789abcdef",
        entries: [
          { name: "README.md", path: "README.md", type: "blob", message: "Docs" },
          { name: "engine.cpp", path: "engine.cpp", type: "blob", message: "Core" },
          { name: "physics", path: "physics", type: "tree", message: "Solver" },
        ],
        counts: {},
      };
    } else if (url.pathname === "/api/repo/alpha/engine/mirrors") {
      body = {
        ok: true,
        mirrors: [{ node: "alpha", status: "online", cloneAvailable: true }],
        summary: { mirrors: 1 },
      };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });

  await page.goto("/alpha/engine");

  const rows = page.locator(
    "[data-repo-tree] [data-dashboard-tree-path], [data-repo-tree] [data-dashboard-blob-path]",
  );
  await expect(rows).toHaveCount(3);
  await page.waitForLoadState("networkidle");
  const treeReadsBeforeSearch = treeRequests.length;

  await page.locator("[data-global-search]").fill("physics");
  await expect(rows).toHaveCount(1);
  await expect(rows.first()).toContainText("physics");

  // Filtering re-renders the listing already fetched - a keystroke must not
  // turn into another mirror read.
  expect(treeRequests.length).toBe(treeReadsBeforeSearch);

  await page.locator("[data-global-search]").fill("");
  await expect(rows).toHaveCount(3);
});

test("header search filters the network page's connected nodes", async ({
  page,
}) => {
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({
      contentType: "text/javascript",
      body: "window.tailwind = {};",
    }),
  );
  await page.route(
    "https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js",
    (route) =>
      route.fulfill({
        contentType: "text/javascript",
        body: "window.lucide = { createIcons() {} };",
      }),
  );
  await page.route("**/dashboard/network", (route) =>
    route.fulfill({
      path: path.resolve(
        __dirname, "..", "..", "public", "dashboard", "network", "index.html",
      ),
      contentType: "text/html; charset=utf-8",
    }),
  );
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = { ok: true, repositories: REPOSITORIES };
    } else if (url.pathname === "/api/network/overview") {
      body = {
        ok: true,
        stats: {
          hosts: 3,
          repos: 3,
          clients: 1,
          onlineNodes: ["mirror6", "mirror7", "workstation"],
        },
        leaderboards: {
          uptime: [
            { name: "mirror6", minutes: 300 },
            { name: "mirror7", minutes: 200 },
            { name: "workstation", minutes: 100 },
          ],
          nodes: [],
        },
        history: { hours: [] },
      };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });

  await page.goto("/dashboard/network");

  const nodeList = page.locator("[data-network-node-list]");
  await expect(nodeList).toContainText("mirror6");
  await expect(nodeList).toContainText("workstation");

  await page.locator("[data-global-search]").fill("workstation");
  await expect(nodeList).toContainText("workstation");
  await expect(nodeList).not.toContainText("mirror6");

  await page.locator("[data-global-search]").fill("");
  await expect(nodeList).toContainText("mirror6");
});

test("a header search with no page matches empties the list without breaking it", async ({
  page,
}) => {
  await stubDashboard(page);
  await page.goto("/dashboard/repos");

  const cards = page.locator("#repoList .repo-card");
  await expect(cards).toHaveCount(REPOSITORIES.length);

  await page.locator("[data-global-search]").fill("no-such-repository");
  await expect(cards).toHaveCount(0);
  // The empty state names the term back rather than saying "no repositories":
  // the list is filtered, not empty.
  await expect(page.locator("#repoList").getByText(
    'No results for "no-such-repository" (clear search)',
    { exact: true },
  )).toHaveCount(1);
  await expect(page.locator("[data-repo-summary]")).toHaveText(
    "0 mirrored repositories",
  );

  await page.locator("[data-global-search]").fill("engine");
  await expect(cards).toHaveCount(1);

  // The page's own repository filter uses the same empty-state path.
  await page.locator("[data-global-search]").fill("");
  await page.locator("#repoSearch").fill("no-such-repository");
  await expect(cards).toHaveCount(0);
  await expect(page.locator("#repoList").getByText(
    'No results for "no-such-repository" (clear search)',
    { exact: true },
  )).toHaveCount(1);
  await expect(page.locator("[data-repo-summary]")).toHaveText(
    "0 mirrored repositories",
  );
});

test("the header ✕ and the empty state's (clear search) both undo the search", async ({
  page,
}) => {
  await stubDashboard(page);
  await page.goto("/dashboard/repos");

  const cards = page.locator("#repoList .repo-card");
  const clear = page.locator("[data-global-search-clear]");
  const shortcut = page.locator("[data-global-search-shortcut]");
  const search = page.locator("[data-global-search]");
  await expect(cards).toHaveCount(REPOSITORIES.length);

  // An empty box has nothing to clear, so the slot keeps showing the "/" hint.
  await expect(clear).toBeHidden();
  await expect(shortcut).toBeVisible();

  await search.fill("atlas");
  await expect(clear).toBeVisible();
  await expect(shortcut).toBeHidden();

  await clear.click();
  await expect(search).toHaveValue("");
  await expect(cards).toHaveCount(REPOSITORIES.length);
  await expect(clear).toBeHidden();
  await expect(shortcut).toBeVisible();
  await expect(page.locator("[data-global-search-page-filter]")).toBeHidden();

  // The link inside the empty state is the same action, reachable without
  // going back up to the header.
  await search.fill("no-such-repository");
  await expect(cards).toHaveCount(0);
  await page.locator("#repoList [data-clear-search]").click();
  await expect(search).toHaveValue("");
  await expect(cards).toHaveCount(REPOSITORIES.length);

  // ...and it clears the page's own filter box too, since that box's term is
  // named in the same message and would otherwise keep the list empty.
  await page.locator("#repoSearch").fill("no-such-repository");
  await expect(cards).toHaveCount(0);
  await page.locator("#repoList [data-clear-search]").click();
  await expect(page.locator("#repoSearch")).toHaveValue("");
  await expect(cards).toHaveCount(REPOSITORIES.length);
});

test("header search filters the repo page's agent sessions", async ({ page }) => {
  const SESSIONS = [
    {
      id: "s-1",
      status: "running",
      provider: "claude-code",
      title: "Rebuild the solver",
      targetNode: "physics-mirror6",
      createdBy: "alpha",
      history: [],
    },
    {
      id: "s-2",
      status: "completed",
      provider: "codex",
      title: "Document the physics map tiles",
      targetNode: "mirror7",
      createdBy: "alpha",
      history: [],
    },
  ];
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alpha",
      email: "alpha@example.test",
      emailVerified: true,
      sessionToken: "test-session",
      status: "active",
    }));
  });
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.tailwind = {};" }),
  );
  await page.route(
    "https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js",
    (route) =>
      route.fulfill({
        contentType: "text/javascript",
        body: "window.lucide = { createIcons() {} };",
      }),
  );
  await page.route("**/alpha/engine/agents", (route) =>
    route.fulfill({ path: REPO_PAGE_PATH, contentType: "text/html; charset=utf-8" }),
  );
  const agentReads = [];
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = { ok: true, repositories: REPOSITORIES };
    } else if (url.pathname.endsWith("/agent-bots")) {
      agentReads.push(url.pathname);
      body = {
        ok: true,
        memberRole: "owner",
        canQueueAgent: true,
        canViewAgentSessions: true,
        sessions: SESSIONS,
      };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });

  await page.goto("/alpha/engine/agents");

  const sessions = page.locator("[data-org-agent-session]");
  await expect(sessions).toHaveCount(2);
  const readsBeforeSearch = agentReads.length;

  const search = page.locator("[data-global-search]");
  await search.fill("physics");
  await expect(sessions).toHaveCount(2);
  // Both rows match, but the title match outranks the node-metadata match.
  await expect(sessions.first()).toContainText("Document the physics map tiles");
  // Filtering redraws sessions already fetched - no second org-agent read.
  expect(agentReads.length).toBe(readsBeforeSearch);

  // A term nothing matches names itself back, with the way out.
  await search.fill("no-such-agent");
  await expect(sessions).toHaveCount(0);
  await expect(page.locator("[data-repo-agents]").getByText(
    'No results for "no-such-agent" (clear search)',
    { exact: true },
  )).toHaveCount(1);

  await page.locator("[data-repo-agents] [data-clear-search]").click();
  await expect(search).toHaveValue("");
  await expect(sessions).toHaveCount(2);
  expect(agentReads.length).toBe(readsBeforeSearch);
});

// Enter in the header box opens the highlighted result. With nothing to open,
// it used to hand the router repoKey({}) — the truthy-but-empty key "/" — which
// became the hostless URL "//" and made the browser throw
// "'https:' is not a valid URL" out of location.assign (adhoc #104). Pressing
// Enter on a term that matches nothing must simply do nothing.
test("Enter with no matching repository neither navigates nor throws", async ({
  page,
}) => {
  const pageErrors = [];
  page.on("pageerror", (error) => pageErrors.push(error.message));

  await stubDashboard(page);
  await page.goto("/dashboard/repos");
  await expect(page.locator("#repoList .repo-card")).toHaveCount(
    REPOSITORIES.length,
  );

  const search = page.locator("[data-global-search]");
  await search.click();
  await search.fill("no-such-repository");
  await expect(page.locator("[data-global-search-result]")).toHaveCount(0);
  await expect(page.locator("[data-global-search-panel]")).toContainText(
    "No repositories match this search.",
  );

  await search.press("Enter");

  // Still on the repos page, with the dropdown and the typed term intact so
  // the search can be corrected rather than started over.
  await expect(page).toHaveURL(/\/dashboard\/repos$/);
  await expect(search).toHaveValue("no-such-repository");
  await expect(page.locator("[data-global-search-panel]")).toBeVisible();
  expect(pageErrors).toEqual([]);

  // A term that does match still opens that repository on Enter.
  await search.fill("atlas");
  await expect(page.locator("[data-global-search-result]")).toHaveCount(1);
  await search.press("Enter");
  await expect(page).toHaveURL(/\/beta\/atlas$/);
  expect(pageErrors).toEqual([]);
});

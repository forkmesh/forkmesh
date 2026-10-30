const { test, expect } = require("@playwright/test");
const path = require("node:path");

const REPO_PAGE_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "public",
  "dashboard",
  "repo.html",
);
const DASHBOARD_JS_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "public",
  "dashboard.js",
);
const DASHBOARD_CSS_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "public",
  "dashboard",
  "tailwind.css",
);

const COMMIT = "0123456789abcdef0123456789abcdef01234567";
const ISSUES = [
  {
    number: 1,
    title: "Fix the parser",
    authorName: "Alice",
    labels: ["bug", "backend"],
    milestone: "v1",
    assignees: ["Sam"],
  },
  {
    number: 2,
    title: "Add a faster renderer",
    authorName: "Bob",
    labels: ["enhancement"],
    milestone: "v2",
    assignees: ["Alex"],
  },
  {
    number: 3,
    title: "Document the API",
    authorName: "Alice",
    labels: ["documentation"],
    milestone: "",
    assignees: [],
  },
  {
    number: 4,
    title: "Repair the dashboard",
    authorName: "Viewer",
    labels: ["bug"],
    milestone: "v2",
    assignees: ["viewer"],
  },
].map((issue) => ({
  ...issue,
  author: `key-${issue.authorName.toLowerCase()}`,
  status: "open",
  priority: 0,
  wantsAgent: issue.number === 2,
  createdAt: 1800000000000 + issue.number,
  updatedAt: 1800000000000 + issue.number,
  events: [
    {
      type: "open",
      id: `open-${issue.number}`,
      title: issue.title,
      body: `Body for issue ${issue.number}`,
      author: `key-${issue.authorName.toLowerCase()}`,
      authorName: issue.authorName,
      ts: 1800000000000 + issue.number,
    },
  ],
}));

async function mockIssuesPage(page, options = {}) {
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "viewer",
      email: "viewer@example.test",
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
    (route) => route.fulfill({
      contentType: "text/javascript",
      body: "window.lucide = { createIcons() {} };",
    }),
  );
  await page.route("**/acme/widgets/issues", (route) =>
    route.fulfill({ path: REPO_PAGE_PATH, contentType: "text/html; charset=utf-8" }),
  );
  await page.route("**/dashboard.js*", (route) =>
    route.fulfill({ path: DASHBOARD_JS_PATH, contentType: "text/javascript; charset=utf-8" }),
  );
  await page.route("**/dashboard/tailwind.css", (route) =>
    route.fulfill({ path: DASHBOARD_CSS_PATH, contentType: "text/css; charset=utf-8" }),
  );
  await page.route("**/api/**", async (route) => {
    const url = new URL(route.request().url());
    const treePath = url.searchParams.get("path") || "";
    const issues = typeof options.issues === "function"
      ? options.issues()
      : ISSUES;
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = {
        ok: true,
        repositories: [
          {
            owner: "acme",
            name: "widgets",
            visibility: "public",
            source: "remote-clone",
            cloneUrl: "https://forkmesh.test/acme/widgets",
            rootCommit: COMMIT,
            commit: COMMIT,
            stateHash: "state",
            issueCount: issues.length,
            openIssues: issues.length,
            closedIssues: 0,
            updatedAt: "2",
          },
        ],
      };
    } else if (url.pathname === "/api/repo/acme/widgets/tree") {
      let entries = [];
      if (treePath === "") {
        entries = [{ type: "tree", name: ".forkmesh" }];
      } else if (treePath === ".forkmesh/issues") {
        if (typeof options.onIssuesTree === "function") {
          await options.onIssuesTree();
        }
        entries = [{ type: "tree", name: "open" }];
      } else if (treePath === ".forkmesh/issues/open") {
        entries = issues.map((issue) => ({
          type: "tree",
          name: String(issue.number),
        }));
      }
      body = {
        ok: true,
        commit: COMMIT,
        entries,
        counts: treePath ? undefined : { openIssues: issues.length, closedIssues: 0 },
      };
    } else if (url.pathname === "/api/repo/acme/widgets/blobs") {
      const byNumber = new Map(issues.map((issue) => [issue.number, issue]));
      const blobs = {};
      url.searchParams.getAll("path").forEach((blobPath) => {
        const match = /issue-(\d+)\.json$/.exec(blobPath);
        const issue = match ? byNumber.get(Number(match[1])) : null;
        if (issue) blobs[blobPath] = { ok: true, content: JSON.stringify(issue) };
      });
      body = { ok: true, blobs };
    } else if (url.pathname === "/api/repo/acme/widgets/branches") {
      body = { ok: true, branches: [{ name: "main", commit: COMMIT }] };
    } else if (url.pathname === "/api/repo/acme/widgets/mirrors") {
      body = { ok: true, mirrors: [], summary: { mirrors: 0 } };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
}

async function issueNumbers(page) {
  return page.locator("[data-repo-issues] [data-repo-record-number]")
    .evaluateAll((rows) => rows
      .map((row) => Number(row.dataset.repoRecordNumber))
      .sort((a, b) => a - b));
}

async function issueOrder(page) {
  return page.locator("[data-repo-issues] [data-repo-record-number]")
    .evaluateAll((rows) => rows.map((row) => Number(row.dataset.repoRecordNumber)));
}

test("issues page uses the all-issues hierarchy and dismissible contribution guide", async ({
  page,
}) => {
  await mockIssuesPage(page);
  await page.goto("/acme/widgets/issues");

  await expect(page.getByRole("heading", { name: "All issues" })).toBeVisible();
  await expect(page.locator("[data-repo-issue-list-header]")).toBeVisible();
  await expect(page.locator('[data-dashboard-issue-filter="open"]')).toContainText("Open4");
  await expect(page.locator('[data-dashboard-issue-filter="closed"]')).toContainText("Closed0");
  await expect(page.locator('[data-repo-record-number="4"] [data-repo-issue-label]')).toHaveText(["bug"]);

  const tip = page.locator("[data-repo-issue-contribution-tip]");
  await expect(tip).toContainText("Want to contribute to acme/widgets?");
  await tip.locator("summary").click();
  await tip.locator('[data-repo-issue-contribution-dismiss="repository"]').click();
  await expect(tip).toHaveCount(0);

  await page.reload();
  await expect(page.locator("[data-repo-issue-contribution-tip]")).toHaveCount(0);
});

test("issue type and sort controls change the visible rows", async ({ page }) => {
  await mockIssuesPage(page);
  await page.goto("/acme/widgets/issues");
  await expect(page.locator("[data-repo-issues] [data-repo-record-number]")).toHaveCount(
    ISSUES.length,
  );

  const type = page.locator('[data-repo-issue-filter="type"]');
  const sort = page.locator('[data-repo-issue-filter="sort"]');
  await expect(type.locator("option")).toHaveText([
    "All types", "Standard issues", "Agent tasks",
  ]);
  await expect(sort.locator("option")).toHaveText([
    "Newest", "Recently updated", "Oldest", "Highest priority", "Most commented",
  ]);

  await sort.selectOption("oldest");
  expect(await issueOrder(page)).toEqual([1, 2, 3, 4]);
  await type.selectOption("agent");
  expect(await issueOrder(page)).toEqual([2]);
});

test("issue author, label, milestone, and assignee filters narrow loaded rows", async ({
  page,
}) => {
  await mockIssuesPage(page);
  await page.goto("/acme/widgets/issues");

  const rows = page.locator("[data-repo-issues] [data-repo-record-number]");
  await expect(rows).toHaveCount(ISSUES.length, { timeout: 15_000 });

  const author = page.locator('[data-repo-issue-filter="author"]');
  const label = page.locator('[data-repo-issue-filter="label"]');
  const milestone = page.locator('[data-repo-issue-filter="milestone"]');
  const assignee = page.locator('[data-repo-issue-filter="assignee"]');

  await expect(author.locator("option")).toHaveText(["Any author", "Alice", "Bob", "Viewer"]);
  await expect(label.locator("option")).toHaveText([
    "Any label", "backend", "bug", "documentation", "enhancement",
  ]);
  await expect(milestone.locator("option")).toHaveText([
    "Any milestone", "No milestone", "v1", "v2",
  ]);
  await expect(assignee.locator("option")).toHaveText([
    "Anyone", "Assigned to me", "Anyone assigned", "Unassigned", "Alex", "Sam", "viewer",
  ]);

  await author.selectOption({ label: "Alice" });
  expect(await issueNumbers(page)).toEqual([1, 3]);

  // Multiple metadata filters combine instead of replacing one another.
  await label.selectOption({ label: "bug" });
  expect(await issueNumbers(page)).toEqual([1]);
  await author.selectOption("");
  expect(await issueNumbers(page)).toEqual([1, 4]);
  await label.selectOption("");

  await milestone.selectOption({ label: "v2" });
  expect(await issueNumbers(page)).toEqual([2, 4]);
  await milestone.selectOption({ label: "No milestone" });
  expect(await issueNumbers(page)).toEqual([3]);
  await milestone.selectOption("");

  await assignee.selectOption({ label: "Sam" });
  expect(await issueNumbers(page)).toEqual([1]);
  await assignee.selectOption({ label: "Unassigned" });
  expect(await issueNumbers(page)).toEqual([3]);
  await assignee.selectOption({ label: "Assigned to me" });
  expect(await issueNumbers(page)).toEqual([4]);
  await assignee.selectOption("");
  expect(await issueNumbers(page)).toEqual([1, 2, 3, 4]);
});

test("issue filters survive refresh and tab re-entry", async ({ page }) => {
  await mockIssuesPage(page);
  await page.goto("/acme/widgets/issues");

  const rows = page.locator("[data-repo-issues] [data-repo-record-number]");
  await expect(rows).toHaveCount(ISSUES.length, { timeout: 15_000 });

  const author = page.locator('[data-repo-issue-filter="author"]');
  const query = page.locator('[data-repo-filter-query="issues"]');

  // A menu choice is written into the search box as a qualifier...
  await author.selectOption({ label: "Alice" });
  await expect(query).toHaveValue("is:issue state:open author:Alice ");
  // ...and a typed search keeps qualifiers and free text together.
  await query.fill("is:issue state:all author:Alice API");
  expect(await issueNumbers(page)).toEqual([3]);

  await page.locator('[data-dashboard-repo-tab="code"]').click();
  await page.locator('[data-dashboard-repo-tab="issues"]').click();

  await expect(rows).toHaveCount(1);
  await expect(author).toHaveValue("Alice");
  await expect(query).toHaveValue(/^is:issue state:all author:Alice API ?$/);
  expect(await issueNumbers(page)).toEqual([3]);
});

test("repository tabs are links: middle-click opens a new browser tab, a plain click stays in-app", async ({
  page,
  context,
}) => {
  await mockIssuesPage(page);
  await page.goto("/acme/widgets/issues");
  await expect(page.locator("[data-repo-issues] [data-repo-record-number]"))
    .toHaveCount(ISSUES.length, { timeout: 15_000 });

  const header = page.locator("[data-repo-github-header]");
  await expect(header.locator('[data-dashboard-repo-tab="code"]')).toHaveAttribute("href", "/acme/widgets");
  for (const tab of ["issues", "pulls", "projects", "discussions", "insights"]) {
    await expect(header.locator(`[data-dashboard-repo-tab="${tab}"]`))
      .toHaveAttribute("href", `/acme/widgets/${tab}`);
  }

  // Middle-click and ctrl-click open the tab's own page in a new browser tab
  // and leave this page on Issues.
  const pulls = header.locator('[data-dashboard-repo-tab="pulls"]');
  for (const click of [{ button: "middle" }, { modifiers: ["Control"] }]) {
    const opened = context.waitForEvent("page");
    await pulls.click(click);
    const newPage = await opened;
    await newPage.waitForURL("**/acme/widgets/pulls", { waitUntil: "commit" });
    await newPage.close();
    await expect(page).toHaveURL(/\/acme\/widgets\/issues$/);
    await expect(header.locator('[data-dashboard-repo-tab="issues"]')).toHaveAttribute("aria-selected", "true");
  }

  // A plain click switches tabs in place without a page load.
  await page.evaluate(() => { window.__sameDocument = true; });
  await pulls.click();
  await expect(page).toHaveURL(/\/acme\/widgets\/pulls$/);
  await expect(pulls).toHaveAttribute("aria-selected", "true");
  expect(await page.evaluate(() => window.__sameDocument)).toBe(true);
});

test("issue views, qualifiers and the New issue menu work like GitHub", async ({ page }) => {
  await mockIssuesPage(page);
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto("/acme/widgets/issues");

  const rows = page.locator("[data-repo-issues] [data-repo-record-number]");
  await expect(rows).toHaveCount(ISSUES.length, { timeout: 15_000 });
  const query = page.locator('[data-repo-filter-query="issues"]');
  const views = page.locator("nav[data-repo-issue-views]");
  const heading = page.locator("[data-repo-issue-view-title]");

  await expect(views.locator('[data-repo-issue-view="issues"]')).toHaveAttribute("aria-current", "true");
  await expect(heading).toHaveText("All issues");

  // Viewer is assigned #4 and opened #4.
  await views.locator('[data-repo-issue-view="assigned"]').click();
  await expect(heading).toHaveText("Assigned to me");
  await expect(query).toHaveValue("is:issue state:open assignee:@me ");
  expect(await issueNumbers(page)).toEqual([4]);

  // A qualifier typed by hand lights up the matching view.
  await query.fill("is:issue state:open author:Viewer");
  await expect(views.locator('[data-repo-issue-view="created"]')).toHaveAttribute("aria-current", "true");
  expect(await issueNumbers(page)).toEqual([4]);

  // Sidebar labels toggle a label filter; views replace the whole search.
  await views.locator('[data-repo-issue-view="issues"]').click();
  await views.locator('[data-repo-issue-label-view="bug"]').click();
  expect(await issueNumbers(page)).toEqual([1, 4]);
  await expect(query).toHaveValue("is:issue state:open label:bug ");
  await views.locator('[data-repo-issue-label-view="bug"]').click();
  expect(await issueNumbers(page)).toEqual([1, 2, 3, 4]);

  // Collapsing the sidebar persists and leaves a button to bring it back.
  await views.locator("[data-repo-issue-views-toggle]").click();
  await expect(views).toBeHidden();
  const reopen = page.locator('[data-repo-issue-views-toggle][aria-label="Show issue views"]');
  await expect(reopen).toBeVisible();
  await page.reload();
  await expect(rows).toHaveCount(ISSUES.length, { timeout: 15_000 });
  await expect(page.locator("nav[data-repo-issue-views]")).toBeHidden();
  await page.locator('[data-repo-issue-views-toggle][aria-label="Show issue views"]').click();
  await expect(page.locator("nav[data-repo-issue-views]")).toBeVisible();

  // Import CSV lives in the New issue menu.
  await page.locator("[data-repo-issue-new-menu] summary").click();
  await expect(page.locator("[data-repo-issue-new-menu] [data-repo-issue-import]")).toBeVisible();
});

test("the header search box does not filter the issues list (issue #673)", async ({
  page,
}) => {
  await mockIssuesPage(page);
  await page.goto("/acme/widgets/issues");

  const rows = page.locator("[data-repo-issues] [data-repo-record-number]");
  await expect(rows).toHaveCount(ISSUES.length, { timeout: 15_000 });

  // Text in the header box (typed for the repo dropdown, or autofilled) used
  // to empty this list with "No results for ..." while its own box sat blank.
  await page.locator("[data-global-search]").fill("no-such-issue");
  await expect(page.locator("[data-global-search-results]")).toBeAttached();
  expect(await issueNumbers(page)).toEqual([1, 2, 3, 4]);
  await expect(page.locator("[data-repo-issues]")).not.toContainText("No results for");

  // The tab's own search box still narrows it, and names only its own terms.
  const query = page.locator('[data-repo-filter-query="issues"]');
  await query.fill("API");
  await expect(rows).toHaveCount(1);
  expect(await issueNumbers(page)).toEqual([3]);
  await query.fill("nothing-matches");
  await expect(page.locator("[data-repo-issues]")).toContainText('No results for "nothing-matches"');
  await page.locator("[data-repo-issues] [data-clear-search]").click();
  await expect(rows).toHaveCount(ISSUES.length);
});

test("issues auto-refresh on page load and the refresh spinner tracks each mirror pull", async ({
  page,
}) => {
  let currentIssues = ISSUES;
  let treeReads = 0;
  const releases = [];
  await mockIssuesPage(page, {
    issues: () => currentIssues,
    onIssuesTree: async () => {
      treeReads += 1;
      await new Promise((resolve) => releases.push(resolve));
    },
  });

  await page.goto("/acme/widgets/issues");
  await expect.poll(() => treeReads).toBe(1);
  const refresh = page.locator("[data-repo-issues-refresh]");
  await expect(refresh).toBeDisabled();
  await expect(refresh).toHaveAttribute("aria-busy", "true");
  await expect(refresh.locator("[data-repo-issues-refresh-label]")).toHaveText(
    "Refreshing",
  );
  await expect(refresh.locator("[data-repo-issues-refresh-icon]")).toHaveClass(
    /animate-spin/,
  );

  releases.shift()();
  await expect(page.locator("[data-repo-issues] [data-repo-record-number]")).toHaveCount(
    ISSUES.length,
  );
  await expect(refresh).toBeEnabled();
  await expect(refresh).toHaveAttribute("aria-busy", "false");
  await expect(refresh.locator("[data-repo-issues-refresh-label]")).toHaveText(
    "Refresh",
  );

  currentIssues = [
    ...ISSUES,
    {
      ...ISSUES[0],
      number: 5,
      title: "New issue from another mirror",
      createdAt: 1800000000005,
      updatedAt: 1800000000005,
      events: [{
        ...ISSUES[0].events[0],
        id: "open-5",
        title: "New issue from another mirror",
        ts: 1800000000005,
      }],
    },
  ];
  await refresh.click();
  await expect.poll(() => treeReads).toBe(2);
  await expect(refresh).toBeDisabled();
  await expect(refresh.locator("[data-repo-issues-refresh-icon]")).toHaveClass(
    /animate-spin/,
  );
  releases.shift()();

  await expect(page.locator('[data-repo-record-number="5"]')).toContainText(
    "New issue from another mirror",
  );
  await expect(refresh).toBeEnabled();
  await expect(refresh.locator("[data-repo-issues-refresh-icon]")).not.toHaveClass(
    /animate-spin/,
  );
});

test("opening an issue paints its page from the clicked row before the mirror answers", async ({
  page,
}) => {
  await mockIssuesPage(page);
  let releaseBlob;
  const blobHeld = new Promise((resolve) => { releaseBlob = resolve; });
  await page.route((url) => url.pathname === "/api/repo/acme/widgets/blob", async (route) => {
    await blobHeld;
    const issue = ISSUES[0];
    const path = new URL(route.request().url()).searchParams.get("path") || "";
    return route.fulfill({
      status: path.includes("/open/1/") ? 200 : 404,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(path.includes("/open/1/")
        ? { ok: true, content: JSON.stringify(issue) }
        : { ok: false, error: "not_found" }),
    });
  });
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto("/acme/widgets/issues");
  await expect(page.locator("[data-repo-issues] [data-repo-record-number]")).toHaveCount(
    ISSUES.length, { timeout: 15_000 },
  );
  await page.route((url) => url.pathname === "/api/repo/acme/widgets/blobs"
    && url.searchParams.getAll("path").some((path) => path.endsWith("/1/issue-1.json")), async (route) => {
    await blobHeld;
    return route.fallback();
  });

  await page.locator('[data-repo-issues] [data-repo-record-number="1"]').click();
  await expect(page).toHaveURL(/\/acme\/widgets\/issues\/1$/);
  const skeleton = page.locator('[data-repo-issue-skeleton="1"]');
  await expect(skeleton).toBeVisible();
  await expect(skeleton.locator("h1")).toHaveText("Fix the parser #1");
  await expect(skeleton).toContainText("Alice opened");
  await expect(skeleton.locator(".issue-label")).toHaveText(["bug", "backend"]);
  // The list's own chrome has already stepped aside.
  await expect(page.locator("nav[data-repo-issue-views]")).toBeHidden();
  await expect(page.locator('[data-repo-filter-query="issues"]')).toBeHidden();

  releaseBlob();
  await expect(page.locator('[data-repo-record-detail="issues"] [data-repo-issue-title-heading]'))
    .toHaveText("Fix the parser #1", { timeout: 15_000 });
  await expect(skeleton).toHaveCount(0);

  // With no Back button, the Issues tab returns to the list and its chrome.
  await page.locator('[data-repo-github-header] [data-dashboard-repo-tab="issues"]').click();
  await expect(page).toHaveURL(/\/acme\/widgets\/issues$/);
  await expect(page.locator("[data-repo-issues] [data-repo-record-number]")).toHaveCount(
    ISSUES.length, { timeout: 15_000 },
  );
  await expect(page.locator("nav[data-repo-issue-views]")).toBeVisible();
  await expect(page.locator('[data-repo-record-detail="issues"]')).toHaveCount(0);

  // Browser Back returns to the issue page.
  await page.goBack();
  await expect(page.locator('[data-repo-record-detail="issues"] [data-repo-issue-title-heading]'))
    .toHaveText("Fix the parser #1", { timeout: 15_000 });
  await expect(page.locator("nav[data-repo-issue-views]")).toBeHidden();
});

test.describe("on a phone", () => {
  test.use({ viewport: { width: 390, height: 844 }, hasTouch: true, isMobile: true });

  test("issue toolbar buttons share one height and the list header stays compact", async ({
    page,
  }) => {
    await mockIssuesPage(page);
    await page.goto("/acme/widgets/issues");
    await expect(page.locator("[data-repo-issues] [data-repo-record-number]")).toHaveCount(
      ISSUES.length,
    );
    expect(await page.evaluate(() => matchMedia("(pointer: coarse)").matches)).toBe(true);

    const heights = await page.evaluate(() => [
      "[data-repo-issues-refresh]",
      "[data-repo-issue-new]",
      "[data-repo-issue-new-menu] summary",
      "[data-repo-issue-view-select]",
    ].map((selector) => document.querySelector(selector).getBoundingClientRect().height));
    expect(new Set(heights).size).toBe(1);
    const tops = await page.evaluate(() => [
      "[data-repo-issue-view-select]",
      "[data-repo-issues-refresh]",
      "[data-repo-issue-new]",
    ].map((selector) => document.querySelector(selector).getBoundingClientRect().top));
    expect(new Set(tops).size).toBe(1);

    const header = page.locator("[data-repo-issue-list-header]");
    const filters = page.locator("[data-repo-issue-list-filters]");
    const filterBox = await filters.boundingBox();
    expect(filterBox.height).toBeLessThanOrEqual(32);
    expect((await header.boundingBox()).height).toBeLessThanOrEqual(80);
    expect(await filters.evaluate((node) => node.scrollWidth > node.clientWidth)).toBe(true);
    expect((await page.locator('[data-dashboard-issue-filter="open"]').boundingBox()).height)
      .toBeLessThanOrEqual(32);

    const row = page.locator('[data-repo-issues] a[data-repo-record-number="3"]');
    expect((await row.boundingBox()).height).toBeLessThanOrEqual(56);
    await expect(row.locator("[data-repo-issue-row-title]")).toHaveCSS("font-size", "14px");
  });
});

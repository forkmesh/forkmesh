const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const REPO_PAGE_PATH = path.join(PUBLIC, "dashboard", "repo.html");
const DASHBOARD_JS_PATH = path.join(PUBLIC, "dashboard.js");
const DASHBOARD_CSS_PATH = path.join(PUBLIC, "dashboard", "tailwind.css");

// Contributor rows in Insights name a ForkMesh account whenever the git author
// name IS one (that is the identity the desktop and the mirror intake write),
// and stay plain text for every git identity that resolves to nobody — an
// unresolved name must never be guessed onto someone else's profile (#636).
test("Insights links resolvable contributors to their ForkMesh profiles", async ({
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
  await page.route("**/acme/space/insights", (route) =>
    route.fulfill({
      path: REPO_PAGE_PATH,
      contentType: "text/html; charset=utf-8",
    }),
  );
  await page.route("**/dashboard.js*", (route) =>
    route.fulfill({
      path: DASHBOARD_JS_PATH,
      contentType: "text/javascript; charset=utf-8",
    }),
  );
  await page.route("**/dashboard/tailwind.css", (route) =>
    route.fulfill({
      path: DASHBOARD_CSS_PATH,
      contentType: "text/css; charset=utf-8",
    }),
  );

  // Ordered oldest-resolving last: the upgrade pass walks the rows in order,
  // so mirror23's link appearing proves every earlier row was decided too.
  const commits = [
    ...Array.from({ length: 4 }, (_, index) => ({
      hash: `d${index}`,
      author: "ForkMesh Deploy",
      date: "2026-08-04T00:00:00Z",
    })),
    ...Array.from({ length: 3 }, (_, index) => ({
      hash: `g${index}`,
      author: "ghost",
      date: "2026-08-03T00:00:00Z",
    })),
    ...Array.from({ length: 2 }, (_, index) => ({
      hash: `p${index}`,
      author: "privateuser",
      date: "2026-08-02T00:00:00Z",
    })),
    { hash: "j0", author: "jett", date: "2026-08-01T00:00:00Z" },
    { hash: "m0", author: "mirror23", date: "2026-07-31T00:00:00Z" },
  ];
  const accounts = {
    // A live user account.
    jett: {
      ok: true, exists: true, available: false, name: "jett", kind: "user",
      status: "active",
    },
    // Node accounts have public /@name pages of their own.
    mirror23: {
      ok: true, exists: true, available: false, name: "mirror23", kind: "node",
      status: "active", owner: "jett",
    },
    // Nobody holds this name.
    ghost: { ok: true, exists: false, available: true, name: "ghost" },
    // Taken, but the profile is private: there is no public page to open.
    privateuser: {
      ok: true, exists: true, available: false, name: "privateuser",
      profilePrivate: true,
    },
  };
  const fullContributors = [
    { name: "ForkMesh Deploy", commits: 40, activityWeeks: [...Array(51).fill(0), 4] },
    { name: "ghost", commits: 30, activityWeeks: [...Array(50).fill(0), 2, 1] },
    { name: "privateuser", commits: 20, activityWeeks: Array(52).fill(0) },
    { name: "jett", commits: 6, activityWeeks: [...Array(51).fill(0), 1] },
    { name: "mirror23", commits: 4, activityWeeks: [...Array(51).fill(0), 1] },
    ...Array.from({ length: 18 }, (_, index) => ({
      name: `Historical Person ${index + 1}`,
      commits: 1,
      activityWeeks: Array(52).fill(0),
    })),
  ];

  const accountLookups = [];
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = {
        ok: true,
        repositories: [
          {
            owner: "acme",
            name: "space",
            visibility: "public",
            source: "local-node",
            defaultBranch: "main",
            branch: "main",
            live: true,
            cloneUrl: "https://forkmesh.test/acme/space",
            rootCommit: "root",
            updatedAt: "2",
          },
        ],
      };
    } else if (url.pathname === "/api/repo/acme/space/history") {
      body = { ok: true, commit: "head", commits };
    } else if (url.pathname === "/api/repo/acme/space/stats") {
      body = {
        ok: true,
        commit: "head",
        commitCount: 118,
        contributorCount: 23,
        contributorsTruncated: false,
        contributors: fullContributors,
        fileCount: 4936,
        totalBytes: 104857600,
        extensions: {
          py: { bytes: 13_000_000, files: 200 },
          cpp: { bytes: 13_000_000, files: 100 },
          js: { bytes: 8_000_000, files: 80 },
        },
        counts: {
          issues: 0, openIssues: 0, closedIssues: 0,
          pulls: 17, openPulls: 4, mergedPulls: 13,
          closedPulls: 13, closedUnmergedPulls: 0,
        },
      };
    } else if (url.pathname.startsWith("/api/accounts/")) {
      const name = decodeURIComponent(url.pathname.slice("/api/accounts/".length));
      accountLookups.push(name);
      body = accounts[name] || { ok: true, exists: false, available: true, name };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });

  await page.goto("/acme/space/insights");

  const insights = page.locator("[data-repo-insights]");
  // Every contributor is listed; the rows resolve in order, so mirror23's link
  // (the last row) appearing means every earlier row has been decided.
  await expect(insights).toContainText("ForkMesh Deploy");
  const mirrorLink = insights.locator('a[href$="/@mirror23"]');
  await expect(mirrorLink).toHaveText("mirror23");
  await expect(mirrorLink).toHaveAttribute("title", "ForkMesh profile @mirror23");
  // The account name itself is the link, not a separate badge beside it.
  await expect(insights.locator('a[href$="/@jett"]')).toHaveText("jett");

  // Complete stats, rather than the 60-row history window, drives the page.
  await expect(insights.locator("[data-repo-insights-contributor-row]")).toHaveCount(23);
  await expect(insights.locator('[data-repo-insights-metric="contributors"]')).toContainText("23");
  await expect(insights.locator('[data-repo-insights-metric="files"]')).toContainText("4,936");
  await expect(insights.locator('[data-repo-insights-metric="code-size"]')).toContainText("100.0 MB");
  await expect(insights.locator('[data-repo-insights-metric="pull-requests"]')).toContainText("4 open / 13 merged / 0 closed");
  await expect(insights.locator("#repo-insights-languages")).toHaveText("Languages");
  await expect(insights).toContainText("Historical Person 18");

  // Git identities that resolve to nobody stay plain text: the deploy box's
  // "ForkMesh Deploy", a handle-shaped name no account holds, and a taken name
  // whose profile is private (there is no public page to open).
  await expect(insights.locator("a", { hasText: "ForkMesh Deploy" })).toHaveCount(0);
  await expect(insights).toContainText("ghost");
  await expect(insights.locator('a[href$="/@ghost"]')).toHaveCount(0);
  await expect(insights).toContainText("privateuser");
  await expect(insights.locator('a[href$="/@privateuser"]')).toHaveCount(0);

  // A name that cannot be a handle never costs a lookup, and each handle is
  // resolved exactly once.
  expect(accountLookups).toEqual(["ghost", "privateuser", "jett", "mirror23"]);
});

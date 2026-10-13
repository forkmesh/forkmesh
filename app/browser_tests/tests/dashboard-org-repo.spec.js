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

test("organization repository alias resolves its linked mirror catalog group", async ({
  page,
}) => {
  const requestedPaths = [];
  await page.setViewportSize({ width: 1760, height: 1200 });
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.dashboard.theme", "light");
  });
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
  await page.route("**/forkmesh/forkmesh", (route) =>
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
  await page.route("https://avatars.example.test/**", (route) =>
    route.fulfill({
      status: 200,
      contentType: "image/svg+xml",
      body: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32"><circle cx="16" cy="16" r="16" fill="#24292f"/></svg>',
    }),
  );
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    requestedPaths.push(url.pathname);
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = {
        ok: true,
        repositories: [
          {
            owner: "mirror3",
            name: "forkmesh",
            visibility: "public",
            source: "remote-clone",
            cloneUrl: "https://forkmesh.test/mirror3/forkmesh",
            description: "ForkMesh keeps source code available across independent hosts.",
            rootCommit: "root",
            stateHash: "state",
            updatedAt: "2",
          },
          {
            owner: "mirror2",
            name: "forkmesh",
            visibility: "public",
            source: "remote-clone",
            cloneUrl: "https://forkmesh.test/mirror2/forkmesh",
            description: "ForkMesh keeps source code available across independent hosts.",
            rootCommit: "root",
            stateHash: "state",
            updatedAt: "1",
          },
        ],
      };
    } else if (url.pathname === "/api/orgs/forkmesh/repos") {
      body = {
        ok: true,
        repos: [{ node: "mirror2", repo: "forkmesh" }],
      };
    } else if (url.pathname === "/api/repo/forkmesh/forkmesh/tree") {
      if (url.searchParams.get("path") === ".forkmesh/releases") {
        body = {
          ok: true,
          entries: [{ name: "stable", type: "tree" }],
        };
      } else {
        body = {
          ok: true,
          commit: "0123456789abcdef",
          entries: [
            {
              name: "src",
              path: "src",
              type: "tree",
              commit: "0123456789abcdef",
              message: "Organize source",
            },
            {
              name: "README.md",
              path: "README.md",
              type: "blob",
              size: 42,
              commit: "0123456789abcdef",
              message: "Document the mesh",
            },
          ],
          counts: { commits: 21418, mirrors: 1 },
        };
      }
    } else if (url.pathname === "/api/repo/forkmesh/forkmesh/blob") {
      body = {
        ok: true,
        content: "# ForkMesh\n\nThe repository file preview works.\n",
      };
    } else if (url.pathname === "/api/repo/forkmesh/forkmesh/about") {
      body = {
        ok: true,
        fediverse: {
          enabled: true,
          followers: 4,
          followersList: ["Ada", "Ben", "Cy", "Dee"].map((name) => ({
            name,
            handle: `@${name.toLowerCase()}@social.example`,
            avatarUrl: `https://avatars.example.test/${name.toLowerCase()}.svg`,
            profileUrl: `https://social.example/@${name.toLowerCase()}`,
          })),
        },
      };
    } else if (url.pathname === "/api/repo/forkmesh/forkmesh/blobs") {
      const releasePath = ".forkmesh/releases/stable/release.json";
      body = {
        ok: true,
        blobs: {
          [releasePath]: {
            content: JSON.stringify({ tag: "v1.2.3", published_at: 1796158800000 }),
          },
        },
      };
    } else if (url.pathname === "/api/repo/forkmesh/forkmesh/sizes") {
      body = {
        ok: true,
        size: 42,
        fileCount: 1,
        children: [{ name: "src", type: "directory", size: 42, children: [] }],
      };
    } else if (url.pathname === "/api/repo/forkmesh/forkmesh/stats") {
      body = { ok: true, fileCount: 2, extensions: { md: { bytes: 42 } }, contributors: [] };
    } else if (url.pathname === "/api/repo/forkmesh/forkmesh/mirrors") {
      body = {
        ok: true,
        mirrors: [
          {
            node: "mirror2",
            status: "online",
            cloneAvailable: true,
            version: "0.7.13",
            commit: "0123456789abcdef",
            branch: "main",
            activity: "serving",
            endpoint: "https://mirror2.forkmesh.test",
            endpointIntegrity: "ok",
            checkedAt: 1796158800000,
            lastSync: 1796158800000,
            latencyMs: 24,
            region: "us-east",
            operations: ["clone", "browse", "website"],
            sizeBytes: 507510784,
            issueCount: 92,
            commitCount: 21418,
            branchCount: 69,
            pullCount: 32,
            discussionCount: 2,
            worktreeCount: 3,
            clonesServed: 1126,
            websiteServed: 29829,
            artifactCount: 6,
          },
          {
            node: "mirror3",
            status: "offline",
            cloneAvailable: false,
            version: "0.7.13",
            commit: "0123456789abcdef",
            branch: "main",
            lastSync: 1796072400000,
            issueCount: 92,
            commitCount: 21418,
            branchCount: 69,
            pullCount: 32,
            discussionCount: 2,
            artifactCount: 6,
          },
          {
            node: "edge-mirror-with-a-long-machine-name",
            status: "online",
            cloneAvailable: true,
            version: "0.7.8",
            commit: "fedcba9876543210",
            branch: "main",
            activity: "syncing",
            endpoint: "https://edge-mirror-with-a-long-machine-name.example.test",
            endpointIntegrity: "unknown",
            checkedAt: 1796158740000,
            lastSync: 1796158680000,
            latencyMs: 195,
            region: "us-west",
            operations: ["clone", "browse"],
            sizeBytes: 506252288,
            issueCount: 92,
            commitCount: 21417,
            branchCount: 68,
            pullCount: 31,
            discussionCount: 2,
            worktreeCount: 2,
            clonesServed: 188,
            websiteServed: 4629,
            artifactCount: 0,
          },
        ],
        // Simulate a catalog summary that has not caught up with the live
        // mirror membership response. The Mirrors badge must follow the list.
        summary: { mirrors: 1 },
      };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });

  await page.goto("/forkmesh/forkmesh");

  await expect(
    page.getByRole("heading", { name: "forkmesh/forkmesh", exact: true }),
  ).toBeVisible();
  await expect(page.locator("[data-repo-header-visibility]")).toHaveText("public");
  await expect(page.locator("[data-repo-tree]")).toContainText("README.md");
  await page.locator('[data-repo-tree] [data-dashboard-blob-path="README.md"]').click();
  await expect(page.locator("[data-repo-file-preview]")).toContainText(
    "The repository file preview works.",
  );
  await expect(page.locator('[data-repo-file-mode="preview"]')).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  await page.locator('[data-repo-file-mode="code"]').click();
  await expect(page.locator("[data-repo-file-code]")).toContainText("# ForkMesh");
  await page.locator('[data-dashboard-repo-tab="code"]').click();
  await expect(page.locator("[data-repo-availability-status]")).toHaveText(
    "served by mirror",
  );
  await expect(page.locator("[data-repo-github-header]")).not.toContainText(
    "ForkMesh keeps source code available",
  );
  await expect(page.locator("[data-repo-about-description]")).toContainText(
    "ForkMesh keeps source code available",
  );
  await expect(page.locator("[data-repo-product-promise]")).toHaveCount(0);
  for (const tab of ["commits", "sizemap", "releases"]) {
    await expect(
      page.locator(`[role="tab"][data-dashboard-repo-tab="${tab}"]`),
    ).toHaveCount(0);
  }
  await expect(
    page.locator('[role="tab"][data-dashboard-repo-tab="mirrors"]'),
  ).toHaveCount(0);
  const aboutActions = page.locator("[data-repo-about-actions]");
  await expect(aboutActions).toBeVisible();
  for (const label of ["Watch", "Fork", "Star", "Mirrors"]) {
    await expect(aboutActions.getByText(label, { exact: true })).toBeVisible();
  }
  await expect(
    aboutActions.locator('[data-dashboard-repo-count="mirrors"]'),
  ).toHaveText("3");
  await expect(page.locator("[data-dashboard-history-button]")).toHaveAttribute(
    "href",
    "/forkmesh/forkmesh/commits",
  );
  await expect(
    page.locator('[data-dashboard-history-button] [data-dashboard-repo-count="commits"]'),
  ).toHaveText("21,418");
  await expect(
    page.locator('[role="tab"][data-dashboard-repo-tab="commits"] [data-dashboard-repo-tab-count]'),
  ).toHaveCount(0);
  await expect(page.locator('[data-repo-tree] [data-lucide="folder"]')).toHaveCount(1);
  await expect(page.locator('[data-repo-tree] [data-lucide="file"]')).toHaveCount(1);
  await expect(page.locator("[data-repo-social-follower-avatars]")).toBeVisible();
  await expect(page.locator("[data-repo-social-follower-avatars] > *")).toHaveCount(4);
  await expect(page.locator("[data-repo-social-follower-avatars] img")).toHaveCount(4);
  await expect(page.locator("[data-repo-social-followers]")).toHaveText("4");

  const releasesLink = page.locator("[data-dashboard-releases-button]");
  await expect(releasesLink).toBeVisible();
  await expect(releasesLink).toHaveAttribute("href", "/forkmesh/forkmesh/releases");
  await releasesLink.click();
  await expect(page.locator('[data-dashboard-repo-tab-panel="releases"]')).toBeVisible();
  await page.locator('[data-dashboard-repo-tab="code"]').click();

  const sizeMapLink = page.locator("[data-repo-about-sizemap-open]");
  await expect(sizeMapLink).toBeVisible();
  await sizeMapLink.click();
  await expect(page.locator('[data-dashboard-repo-tab-panel="sizemap"]')).toBeVisible();
  await page.locator('[data-dashboard-repo-tab="code"]').click();

  const appHeaderBox = await page.locator("[data-app-header]").boundingBox();
  const repoTabsBox = await page.locator("[data-repo-github-header]").boundingBox();
  const firstRepoTabBox = await page.locator('[data-dashboard-repo-tab="code"]').boundingBox();
  const contentGridBox = await page.locator("[data-repo-content-grid]").boundingBox();
  expect(appHeaderBox).not.toBeNull();
  expect(repoTabsBox).not.toBeNull();
  expect(firstRepoTabBox).not.toBeNull();
  expect(contentGridBox).not.toBeNull();
  expect(Math.abs(repoTabsBox.y - (appHeaderBox.y + appHeaderBox.height))).toBeLessThanOrEqual(1);
  expect(Math.abs(repoTabsBox.x - appHeaderBox.x)).toBeLessThanOrEqual(1);
  expect(Math.abs(repoTabsBox.width - appHeaderBox.width)).toBeLessThanOrEqual(1);
  expect(firstRepoTabBox.x).toBeLessThanOrEqual(32);
  expect(contentGridBox.width).toBeLessThanOrEqual(1280);
  await expect(page.locator("[data-app-header]")).toHaveCSS("background-color", "rgb(246, 248, 250)");
  await expect(page.locator("[data-repo-github-header]")).toHaveCSS("background-color", "rgb(246, 248, 250)");
  const repoHeaderClasses = await page.locator("[data-repo-github-header]").getAttribute("class");
  expect(repoHeaderClasses).not.toContain("rounded-t-lg");
  expect(repoHeaderClasses).not.toContain(" border ");
  await expect(page.locator("[data-repo-detail]")).not.toContainText(
    "was not found",
  );
  expect(requestedPaths).toContain("/api/orgs/forkmesh/repos");
  expect(requestedPaths).toContain("/api/repo/forkmesh/forkmesh/tree");
  await expect(page).toHaveURL(/\/forkmesh\/forkmesh$/);

  await page.locator('[data-dashboard-repo-tab="mirrors"]').click();
  const mirrorPanel = page.locator('[data-dashboard-repo-tab-panel="mirrors"]');
  await expect(mirrorPanel).toBeVisible();
  const mirrorTable = page.locator("[data-mirror-table]");
  const mirrorRows = page.locator("[data-mirror-row]");
  await expect(mirrorTable).toBeVisible();
  await expect(mirrorRows).toHaveCount(3);
  await expect(mirrorRows.first()).toContainText("mirror2");
  await expect(mirrorTable.locator('thead th[title="Node"] [data-lucide="server"]')).toBeVisible();
  await expect(mirrorTable.locator('thead th[title="Revision"] [data-lucide="git-commit-horizontal"]')).toBeVisible();
  await expect(mirrorTable.locator('thead th[title="Website requests"] [data-lucide="mouse-pointer-click"]')).toBeVisible();

  const desktopRowBoxes = await mirrorRows.evaluateAll((rows) =>
    rows.map((row) => row.getBoundingClientRect().toJSON()),
  );
  expect(desktopRowBoxes[1].y).toBeGreaterThan(desktopRowBoxes[0].y);
  expect(desktopRowBoxes[2].y).toBeGreaterThan(desktopRowBoxes[1].y);
  expect(desktopRowBoxes.every((box) => box.height <= 28)).toBe(true);
  expect(
    await mirrorTable.evaluate((table) => getComputedStyle(table).borderTopWidth),
  ).toBe("0px");
  expect(
    await mirrorPanel.evaluate((panel) => getComputedStyle(panel).marginTop),
  ).toBe("0px");
  expect(
    await mirrorPanel.evaluate((panel) => panel.scrollWidth <= panel.clientWidth + 1),
  ).toBe(true);

  await page.setViewportSize({ width: 390, height: 844 });
  const mobileRowBoxes = await mirrorRows.evaluateAll((rows) =>
    rows.map((row) => row.getBoundingClientRect().toJSON()),
  );
  expect(mobileRowBoxes[1].y).toBeGreaterThan(mobileRowBoxes[0].y);
  expect(mobileRowBoxes.every((box) => box.height <= 28)).toBe(true);
  expect(
    await mirrorPanel.evaluate((panel) => panel.scrollWidth <= panel.clientWidth + 1),
  ).toBe(true);
});

// Issue #479: a signed-out visitor can read every public repository view
// without being sent to registration. Nothing is stored in localStorage, so
// the page boots exactly as a first-time visitor's browser would.
const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC_DIR = path.resolve(__dirname, "..", "..", "public");
const REPO_PAGE_PATH = path.join(PUBLIC_DIR, "dashboard", "repo.html");
const DASHBOARD_JS_PATH = path.join(PUBLIC_DIR, "dashboard.js");
const DASHBOARD_CSS_PATH = path.join(PUBLIC_DIR, "dashboard", "tailwind.css");

const PULL_MD = `---
schema: forkmesh-pull-v1
number: 93
title: Public pull request
base: main
head: contributor:feature
status: open
ts: 1796158800000
author: pubkey
authorName: contributor
---

Readable without an account.
`;

const PUBLIC_VIEWS = [
  "/acme/widgets",
  "/acme/widgets/issues",
  "/acme/widgets/pulls",
  "/acme/widgets/pull/93",
  "/acme/widgets/releases",
  "/acme/widgets/mirrors",
  "/acme/widgets/branches",
  "/acme/widgets/commits",
];

async function mockPublicRepository(page) {
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.tailwind = {};" }),
  );
  await page.route("https://cdn.jsdelivr.net/**", (route) =>
    route.fulfill({
      contentType: "text/javascript",
      body: "window.lucide = { createIcons() {} };",
    }),
  );
  await page.route("**/acme/widgets**", (route) =>
    route.fulfill({ path: REPO_PAGE_PATH, contentType: "text/html; charset=utf-8" }),
  );
  await page.route("**/dashboard.js*", (route) =>
    route.fulfill({ path: DASHBOARD_JS_PATH, contentType: "text/javascript; charset=utf-8" }),
  );
  await page.route("**/dashboard/tailwind.css", (route) =>
    route.fulfill({ path: DASHBOARD_CSS_PATH, contentType: "text/css; charset=utf-8" }),
  );
  // A registration page must never be requested by a read; answer it with a
  // marker so an accidental bounce is unmistakable in the failure output.
  await page.route(/\/(login|signup)(\?|$)/, (route) =>
    route.fulfill({ contentType: "text/html", body: "<title>registration</title>" }),
  );
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
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
            rootCommit: "root",
            stateHash: "state",
            updatedAt: "2",
          },
        ],
      };
    } else if (url.pathname === "/api/repo/acme/widgets/branches") {
      body = {
        ok: true,
        branches: [{ name: "main", commit: "2222222222222222222222222222222222222222" }],
      };
    } else if (url.pathname === "/api/repo/acme/widgets/blob") {
      if (url.searchParams.get("path") === ".forkmesh/pulls/93/pull.md") {
        body = { ok: true, content: PULL_MD };
      } else {
        return route.fulfill({
          status: 404,
          contentType: "application/json; charset=utf-8",
          body: JSON.stringify({ ok: false, error: "not_found" }),
        });
      }
    } else if (url.pathname === "/api/repo/acme/widgets/tree") {
      const entries = url.searchParams.get("path") === ".forkmesh/pulls"
        ? [{ type: "tree", name: "93" }]
        : [];
      body = { ok: true, commit: "1111111111111111111111111111111111111111", entries };
    } else if (url.pathname.startsWith("/api/session") || url.pathname.startsWith("/api/account")) {
      // Signed out: session probes answer the way the Worker does for a guest.
      return route.fulfill({
        status: 401,
        contentType: "application/json; charset=utf-8",
        body: JSON.stringify({ ok: false, error: "unauthorized" }),
      });
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
}

for (const view of PUBLIC_VIEWS) {
  test(`a signed-out visitor reads ${view} without a registration redirect`, async ({ page }) => {
    await mockPublicRepository(page);
    const visited = [];
    page.on("framenavigated", (frame) => {
      if (frame === page.mainFrame()) visited.push(new URL(frame.url()).pathname);
    });

    await page.goto(view);
    // Give the boot path (session read, catalog load, tab loader) time to run
    // any redirect it would make before judging where the page ended up.
    await expect(page.locator("[data-guest-auth-link]")).toBeVisible({ timeout: 15_000 });
    await page.waitForTimeout(750);

    expect(new URL(page.url()).pathname).toBe(view);
    expect(visited.filter((pathname) => /^\/(login|signup)\b/.test(pathname))).toEqual([]);
    await expect(page).not.toHaveTitle("registration");
  });
}

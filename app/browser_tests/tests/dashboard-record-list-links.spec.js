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

const PULL_COMMIT = "1111111111111111111111111111111111111111";

const PULL_MD = `---
schema: forkmesh-pull-v1
number: 93
title: Refactor homepage with hero, trust metrics, and agent visuals
base: main
head: mdkaifansari04:refator/homepage
status: open
ts: 1796158800000
author: pubkey
authorName: mdkaifansari04
---

Homepage refresh.
`;

async function mockRepoPage(page) {
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
  await page.route("**/acme/widgets/**", (route) =>
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
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    const blobPath = url.searchParams.get("path") || "";
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
        branches: [
          { name: "main", commit: "2222222222222222222222222222222222222222" },
          { name: "forkmesh/pulls", commit: PULL_COMMIT },
        ],
      };
    } else if (url.pathname === "/api/repo/acme/widgets/blob") {
      if (blobPath === ".forkmesh/pulls/93/pull.md") {
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
      body = { ok: true, commit: PULL_COMMIT, entries };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
}

test("a pull-request row is a link that opens in a new tab on middle-click", async ({
  page,
  context,
}) => {
  await mockRepoPage(page);
  await page.goto("/acme/widgets/pulls");

  const row = page.locator(
    '[data-repo-pulls] [data-repo-record-kind="pulls"][data-repo-record-number="93"]',
  );
  await expect(row).toBeVisible({ timeout: 15_000 });

  // The row addresses the record itself, so the browser's own "open in new
  // tab" affordances (middle-click, ctrl/cmd-click, copy link, right-click
  // menu) all have a URL to work with.
  await expect(row).toHaveJSProperty("tagName", "A");
  await expect(row).toHaveAttribute("href", "/acme/widgets/pulls/93");

  // Middle-click opens the record in a second tab and leaves this one on the
  // list, exactly where the reviewer left off.
  const opened = context.waitForEvent("page");
  await row.click({ button: "middle" });
  const newTab = await opened;
  await expect(page).toHaveURL(/\/acme\/widgets\/pulls$/);
  await expect(page.locator('[data-repo-record-detail="pulls"]')).toHaveCount(0);
  expect(new URL(newTab.url()).pathname).toBe("/acme/widgets/pulls/93");
  await newTab.close();

  // A plain left click still opens the record in place, without the full page
  // reload the bare link would otherwise cause.
  let reloaded = false;
  page.once("load", () => {
    reloaded = true;
  });
  await row.click();
  await expect(page.locator('[data-repo-record-detail="pulls"]')).toBeVisible();
  await expect(page).toHaveURL(/\/acme\/widgets\/pulls\/93$/);
  expect(reloaded).toBe(false);
});

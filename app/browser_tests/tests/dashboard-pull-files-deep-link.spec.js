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
const DAILY_BUDGETS = require(path.resolve(
  __dirname, "..", "..", "..", "www", "docs", "engineering",
  "daily-interface-budgets.json",
));

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

const PULL_PATCH = `diff --git a/index.html b/index.html
--- a/index.html
+++ b/index.html
@@ -1,2 +1,3 @@
 <html>
+<body>hero</body>
 </html>
`;

const INLINE_COMMENT_MD = `---
type: line-comment
id: line-comment-existing
author: reviewer-key
authorName: alex
ts: 1796158900
path: index.html
side: new
line: 2
---

Existing inline feedback.
`;

async function mockRepoPage(page, submitted = []) {
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
      } else if (blobPath === ".forkmesh/pulls/93/changes.patch") {
        body = { ok: true, content: PULL_PATCH };
      } else if (blobPath === ".forkmesh/pulls/93/0001-line-comment.md") {
        body = { ok: true, content: INLINE_COMMENT_MD };
      } else {
        return route.fulfill({
          status: 404,
          contentType: "application/json; charset=utf-8",
          body: JSON.stringify({ ok: false, error: "not_found" }),
        });
      }
    } else if (url.pathname === "/api/repo/acme/widgets/tree") {
      body = blobPath === ".forkmesh/pulls/93"
        ? {
            ok: true,
            commit: PULL_COMMIT,
            entries: [{ name: "0001-line-comment.md", type: "blob" }],
          }
        : { ok: true, commit: PULL_COMMIT, entries: [] };
    } else if (
      url.pathname === "/api/repo/acme/widgets/pulls" &&
      route.request().method() === "POST"
    ) {
      submitted.push(route.request().postDataJSON());
      body = { ok: true };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
}

test("daily interface diff rendering is addressable, readable, and within budget", async ({
  page,
}) => {
  await mockRepoPage(page);

  // The link a reviewer copies from the Files changed review must open there
  // for whoever it is sent to.
  await page.goto("/acme/widgets/pull/93/files");

  const filesTab = page.locator('[data-repo-record-tab="files"]');
  await expect(filesTab).toHaveAttribute("aria-selected", "true", {
    timeout: 15_000,
  });
  await expect(page.locator('[data-repo-record-panel="files"]')).toBeVisible();
  await expect(
    page.locator('[data-repo-record-panel="conversation"]'),
  ).toBeHidden();
  await expect(page.locator("[data-repo-pull-files]")).toContainText(
    "index.html",
  );
  const diffMetric = await page.evaluate(() =>
    (window.ForkMeshInterfaceMetrics || []).findLast((row) => row.name === "diff"));
  const diffBudget = DAILY_BUDGETS.budgets.find((row) => row.id === "daily-diff-render");
  expect(diffMetric).not.toBeNull();
  expect(diffMetric.duration).toBeLessThanOrEqual(diffBudget.remoteMaximum);
  expect(diffMetric.passed).toBe(true);
  await expect(page.locator("[data-repo-pull-diff-row]").first()).toContainText("+");

  // Both sides of the merge name their repository, not just their branch.
  const hero = page.locator("[data-repo-record-hero]");
  await expect(hero).toContainText("into acme/widgets:main");
  await expect(hero).toContainText("from mdkaifansari04/widgets:refator/homepage");

  // Switching sections rewrites the address bar, so the URL always matches
  // what is on screen.
  await page.locator('[data-repo-record-tab="conversation"]').click();
  await expect(page).toHaveURL(/\/acme\/widgets\/pull\/93$/);
  await expect(
    page.locator('[data-repo-record-panel="conversation"]'),
  ).toBeVisible();

  await filesTab.click();
  await expect(page).toHaveURL(/\/acme\/widgets\/pull\/93\/files$/);
  const navigationMetric = await page.evaluate(() =>
    (window.ForkMeshInterfaceMetrics || []).findLast((row) => row.name === "navigation"));
  const navigationBudget = DAILY_BUDGETS.budgets.find((row) => row.id === "daily-navigation");
  expect(navigationMetric.duration).toBeLessThanOrEqual(navigationBudget.remoteMaximum);
  expect(navigationMetric.passed).toBe(true);

  // Back steps between sections of the same record without refetching it.
  await page.goBack();
  await expect(page).toHaveURL(/\/acme\/widgets\/pull\/93$/);
  await expect(
    page.locator('[data-repo-record-panel="conversation"]'),
  ).toBeVisible();
  await expect(page.locator('[data-repo-record-panel="files"]')).toBeHidden();
});

test("line gutters open inline range comments and support immediate or batched submission", async ({
  page,
}) => {
  const submitted = [];
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "web-reviewer",
      sessionToken: "test-session",
    }));
  });
  await mockRepoPage(page, submitted);
  await page.goto("/acme/widgets/pull/93/files");

  const files = page.locator('[data-repo-record-panel="files"]');
  await expect(files).toBeVisible({ timeout: 15_000 });

  // A committed line comment is collapsed behind a small user marker in the
  // gutter, then expands in place beneath the code row.
  const existingMarker = files.locator(
    '[data-repo-pull-inline-thread-toggle]'
  ).first();
  await expect(existingMarker).toBeVisible();
  await existingMarker.click();
  await expect(files.getByText("Existing inline feedback.")).toBeVisible();

  // Dragging down the new-side line numbers selects one range and places the
  // editor in the diff instead of opening a dialog.
  const first = files.locator(
    '[data-repo-pull-diff-line][data-side="new"][data-line="1"]'
  );
  const third = files.locator(
    '[data-repo-pull-diff-line][data-side="new"][data-line="3"]'
  );
  await first.hover();
  await page.mouse.down();
  await third.hover();
  await page.mouse.up();

  const composer = files.locator("[data-repo-pull-inline-comment-form]");
  await expect(composer).toBeVisible();
  await expect(composer).toContainText("new lines 1-3");
  await expect(composer.getByRole("button", { name: "Add to review" })).toBeVisible();
  await expect(composer.getByRole("button", { name: "Comment now" })).toBeVisible();
  await composer.locator("textarea").fill("Please simplify this range.");
  await composer.getByRole("button", { name: "Add to review" }).click();

  const batch = files.locator("[data-repo-pull-review-batch]");
  await expect(batch).toContainText("1 draft comment");
  await expect(files.getByText("Please simplify this range.")).toBeVisible();
  await batch.getByRole("button", { name: "Submit review (1)" }).click();
  await expect.poll(() => submitted.length).toBe(1);
  expect(submitted[0].event).toMatchObject({
    type: "thread-comment",
    path: "index.html",
    side: "new",
    lineStart: 1,
    lineEnd: 3,
    body: "Please simplify this range.",
  });
  await expect(batch).toBeEmpty();

  // A single selected line can bypass the batch and post immediately.
  const second = files.locator(
    '[data-repo-pull-diff-line][data-side="new"][data-line="2"]'
  );
  await second.click();
  const immediateComposer = files.locator("[data-repo-pull-inline-comment-form]");
  await immediateComposer.locator("textarea").fill("Please rename this element.");
  await immediateComposer.getByRole("button", { name: "Comment now" }).click();
  await expect.poll(() => submitted.length).toBe(2);
  expect(submitted[1].event).toMatchObject({
    type: "line-comment",
    path: "index.html",
    side: "new",
    line: 2,
    body: "Please rename this element.",
  });
});

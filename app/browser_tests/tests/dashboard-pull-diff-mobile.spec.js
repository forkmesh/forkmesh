const { test, expect, devices } = require("@playwright/test");
const path = require("node:path");

// Issue #722: the diff viewer on a phone. Emulates a touch-screen iPhone-sized
// viewport (Chromium engine) against the repo page with every network call
// mocked, the way dashboard-pull-files-deep-link.spec.js does on desktop.

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const REPO_PAGE_PATH = path.join(PUBLIC, "dashboard", "repo.html");
const DASHBOARD_JS_PATH = path.join(PUBLIC, "dashboard.js");
const DASHBOARD_CSS_PATH = path.join(PUBLIC, "dashboard", "tailwind.css");

const PULL_COMMIT = "1111111111111111111111111111111111111111";

const PULL_MD = `---
schema: forkmesh-pull-v1
number: 94
title: Touch up the widget renderer
base: main
head: someone:touch-up
status: open
ts: 1796158800000
author: pubkey
authorName: someone
---

Renderer refresh.
`;

// Twelve files of sixty changed lines each, some long enough to wrap on a
// phone, so the page is several screens tall and every block below the fold
// starts out skipped by content-visibility.
function buildPatch() {
  const files = [];
  for (let f = 0; f < 12; f += 1) {
    const name = `src/module_${String(f).padStart(2, "0")}.js`;
    const lines = [`diff --git a/${name} b/${name}`, `--- a/${name}`, `+++ b/${name}`, "@@ -1,40 +1,40 @@"];
    for (let i = 0; i < 20; i += 1) {
      lines.push(` const keep_${i} = ${i};`);
      lines.push(`-const value_${i} = computeSomethingRatherLong(argumentNumberOne, argumentNumberTwo, ${i});`);
      lines.push(`+const value_${i} = computeSomethingRatherLonger(argumentNumberOne, argumentNumberTwo, ${i}, true);`);
    }
    files.push(lines.join("\n"));
  }
  return `${files.join("\n")}\n`;
}

async function mockRepoPage(page) {
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.tailwind = {};" }));
  await page.route("https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.lucide = { createIcons() {} };" }));
  await page.route("**/acme/widgets/**", (route) =>
    route.fulfill({ path: REPO_PAGE_PATH, contentType: "text/html; charset=utf-8" }));
  await page.route("**/dashboard.js*", (route) =>
    route.fulfill({ path: DASHBOARD_JS_PATH, contentType: "text/javascript; charset=utf-8" }));
  await page.route("**/dashboard/tailwind.css", (route) =>
    route.fulfill({ path: DASHBOARD_CSS_PATH, contentType: "text/css; charset=utf-8" }));
  const patch = buildPatch();
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    const blobPath = url.searchParams.get("path") || "";
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = {
        ok: true,
        repositories: [{
          owner: "acme", name: "widgets", visibility: "public", source: "remote-clone",
          cloneUrl: "https://forkmesh.test/acme/widgets", rootCommit: "root", stateHash: "state", updatedAt: "2",
        }],
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
      if (blobPath === ".forkmesh/pulls/94/pull.md") body = { ok: true, content: PULL_MD };
      else if (blobPath === ".forkmesh/pulls/94/changes.patch") body = { ok: true, content: patch };
      else {
        return route.fulfill({
          status: 404,
          contentType: "application/json; charset=utf-8",
          body: JSON.stringify({ ok: false, error: "not_found" }),
        });
      }
    } else if (url.pathname === "/api/repo/acme/widgets/tree") {
      body = {
        ok: true,
        commit: PULL_COMMIT,
        entries: blobPath === ".forkmesh/pulls/94"
          ? [{ name: "pull.md", type: "blob" }, { name: "changes.patch", type: "blob" }]
          : [],
      };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
}

test.use({
  ...devices["iPhone 13"],
  browserName: "chromium",
  defaultBrowserType: "chromium",
});

async function openFiles(page) {
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({ nodeName: "web-reviewer", sessionToken: "test-session" }));
    // A reader who chose side-by-side on a laptop opens the same pull on a phone.
    localStorage.setItem("forkmesh.pull-layout.v1", JSON.stringify({ explorer: "list", diff: "split" }));
  });
  await mockRepoPage(page);
  await page.goto("/acme/widgets/pulls/94/files");
  const files = page.locator('[data-repo-record-panel="files"]');
  await expect(files).toBeVisible({ timeout: 15_000 });
  await expect(files.locator("[data-repo-pull-diff-file]")).toHaveCount(12);
  return files;
}

test("phone diff rows stay compact, unified, and inside the screen", async ({ page }) => {
  const files = await openFiles(page);

  // Side-by-side cannot fit two code columns on a phone: the saved preference
  // is honoured on wide screens only, and the toggle is not offered here.
  await expect(files.locator('[data-repo-pull-diff-grid="split"]')).toHaveCount(0);
  expect(await files.locator('[data-repo-pull-diff-grid="unified"]').count()).toBeGreaterThan(0);
  await expect(files.locator("[data-repo-pull-diff-mode-group]")).toBeHidden();

  // A one-line row is one line tall. The coarse-pointer 44px tap-target rule
  // used to stretch every line-number button, doubling each row.
  const row = files.locator('[data-repo-pull-diff-line][data-side="new"][data-line="1"]').first()
    .locator("xpath=ancestor::*[@data-repo-pull-diff-grid][1]");
  const rowBox = await row.boundingBox();
  expect(rowBox.height).toBeLessThanOrEqual(24);

  // Nothing pushes the page sideways.
  const overflow = await page.evaluate(() => {
    const list = document.querySelector("[data-repo-pull-diff-list]");
    return {
      page: document.documentElement.scrollWidth - document.documentElement.clientWidth,
      list: list.scrollWidth - list.clientWidth,
    };
  });
  expect(overflow.page).toBeLessThanOrEqual(0);
  expect(overflow.list).toBeLessThanOrEqual(0);

  // A finger landing on the gutter must still scroll the page.
  const touchAction = await files.locator("[data-repo-pull-diff-line]").first()
    .evaluate((el) => getComputedStyle(el).touchAction);
  expect(touchAction).not.toBe("none");

  // Inputs are at least 16px so iOS does not zoom the page on focus.
  const filterSize = await files.locator("[data-repo-pull-file-filter]")
    .evaluate((el) => parseFloat(getComputedStyle(el).fontSize));
  expect(filterSize).toBeGreaterThanOrEqual(16);
});

test("tapping line numbers opens the composer and a second tap extends the range", async ({ page }) => {
  const files = await openFiles(page);
  const block = files.locator('[data-repo-pull-diff-file="src/module_00.js"]');

  await block.locator('[data-repo-pull-diff-line][data-side="new"][data-line="2"]').tap();
  const composer = files.locator("[data-repo-pull-inline-comment-form]");
  await expect(composer).toHaveCount(1);
  await expect(composer).toContainText("new line 2");
  await composer.locator("textarea").fill("Draft kept across the range change.");

  await block.locator('[data-repo-pull-diff-line][data-side="new"][data-line="5"]').tap();
  await expect(composer).toHaveCount(1);
  await expect(composer).toContainText("new lines 2-5");
  await expect(composer.locator("textarea")).toHaveValue("Draft kept across the range change.");
  const textareaSize = await composer.locator("textarea")
    .evaluate((el) => parseFloat(getComputedStyle(el).fontSize));
  expect(textareaSize).toBeGreaterThanOrEqual(16);

  await composer.getByRole("button", { name: "Cancel inline comment" }).tap();
  await expect(composer).toHaveCount(0);
});

test("tapping a file in the explorer lands on that file's diff", async ({ page }) => {
  const files = await openFiles(page);
  const target = "src/module_10.js";
  await expect(files.locator("[data-repo-diff-explorer]")).toBeHidden();
  await files.locator(".fm-diff-explorer-show").first().tap();
  await expect(files.locator("[data-repo-diff-explorer]")).toBeVisible();
  await files.locator(`[data-repo-pull-file="${target}"]`).tap();
  await expect(files.locator("[data-repo-diff-explorer]")).toBeHidden();

  const block = files.locator(`[data-repo-pull-diff-file="${target}"]`);
  await expect.poll(async () => {
    const [blockTop, scrollerTop] = await block.evaluate((el) => [
      el.getBoundingClientRect().top,
      el.closest("[data-view]").getBoundingClientRect().top,
    ]);
    return Math.round(blockTop - scrollerTop);
  }, { timeout: 5_000 }).toBeLessThanOrEqual(16);
  // Once the jump settles the explorer agrees with the pane.
  await page.waitForTimeout(400);
  const [blockTop, scrollerTop] = await block.evaluate((el) => [
    el.getBoundingClientRect().top,
    el.closest("[data-view]").getBoundingClientRect().top,
  ]);
  expect(Math.abs(blockTop - scrollerTop)).toBeLessThanOrEqual(16);
  await expect(files.locator(`[data-repo-pull-file="${target}"]`)).toHaveAttribute("aria-current", "true");
});

test.describe("on a desktop mouse", () => {
  test.use({ viewport: { width: 1440, height: 900 }, isMobile: false, hasTouch: false, deviceScaleFactor: 1 });

  test("split stays available and gutter drags still select a range", async ({ page }) => {
    const files = await openFiles(page);
    await expect(files.locator("[data-repo-diff-explorer]")).toBeVisible();
    await files.locator("[data-repo-diff-settings] > summary").click();
    await expect(files.locator("[data-repo-pull-diff-mode-group]")).toBeVisible();
    expect(await files.locator('[data-repo-pull-diff-grid="split"]').count()).toBeGreaterThan(0);
    await files.locator('[data-repo-pull-diff-mode="unified"]').click();
    expect(await files.locator('[data-repo-pull-diff-grid="unified"]').count()).toBeGreaterThan(0);

    const block = files.locator('[data-repo-pull-diff-file="src/module_00.js"]');
    await block.locator('[data-repo-pull-diff-line][data-side="new"][data-line="1"]').hover();
    await page.mouse.down();
    await block.locator('[data-repo-pull-diff-line][data-side="new"][data-line="3"]').hover();
    await page.mouse.up();
    const composer = files.locator("[data-repo-pull-inline-comment-form]");
    await expect(composer).toHaveCount(1);
    await expect(composer).toContainText("new lines 1-3");
    await expect(composer.locator("textarea")).toBeFocused();
  });
});

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
const FILE_PATH = "src/space name#β?.js";
const ACTIVE_REF = "feature/docs";

async function prepareSizeMap(page) {
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({
      contentType: "text/javascript",
      body: "window.tailwind = {};",
    }),
  );
  await page.route("https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js", (route) =>
    route.fulfill({
      contentType: "text/javascript",
      body: "window.lucide = { createIcons() {} };",
    }),
  );
  await page.route("**/acme/space/sizemap", (route) =>
    route.fulfill({
      path: REPO_PAGE_PATH,
      contentType: "text/html; charset=utf-8",
    }),
  );
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    let body = {};
    if (url.pathname === "/api/repositories") {
      body = {
        repositories: [
          {
            owner: "acme",
            name: "space",
            description: "Size-map acceptance fixture",
            defaultBranch: ACTIVE_REF,
            branch: ACTIVE_REF,
            live: true,
            public: true,
          },
        ],
      };
    } else if (url.pathname === "/api/repo/acme/space/sizes") {
      body = {
        ok: true,
        size: 100,
        fileCount: 1,
        children: [
          {
            name: "src",
            type: "directory",
            size: 100,
            children: [
              {
                name: "space name#β?.js",
                path: FILE_PATH,
                type: "file",
                size: 100,
              },
            ],
          },
        ],
      };
    } else if (url.pathname === "/api/repo/acme/space/blob") {
      body = {
        ok: true,
        path: url.searchParams.get("path"),
        content: "export const accepted = true;\n",
        encoding: "utf-8",
      };
    } else if (url.pathname === "/api/repo/acme/space/tree") {
      body = { ok: true, entries: [], counts: {} };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.goto("/acme/space/sizemap");
  const leaf = page.getByRole("button", {
    name: `Open file src/space name#β?.js`,
  });
  await expect(leaf).toBeVisible();
  return leaf;
}

async function expectLeafActivation(page, activate) {
  const requestPromise = page.waitForRequest((request) => {
    const url = new URL(request.url());
    return url.pathname === "/api/repo/acme/space/blob";
  });
  const leaf = await prepareSizeMap(page);
  await activate(leaf);
  const request = await requestPromise;
  const url = new URL(request.url());
  expect(url.searchParams.get("path")).toBe(FILE_PATH);
  expect(url.searchParams.get("ref")).toBe(ACTIVE_REF);
  expect(request.url()).toContain("path=src%2Fspace+name%23%CE%B2%3F.js");
  expect(request.url()).toContain("ref=feature%2Fdocs");
  await expect(page.locator("[data-repo-blob]")).toContainText(
    "export const accepted = true;",
  );
}

test("size-map file leaf opens with a fine-pointer click", async ({ page }) => {
  await expectLeafActivation(page, async (leaf) => {
    const box = await leaf.boundingBox();
    expect(box).not.toBeNull();
    // A full-circle ring's geometric center is the sunburst hole; activate a
    // point in the visible outer arc, as a pointer user would.
    await leaf.click({ position: { x: box.width / 2, y: 10 } });
  });
});

test("size-map file leaf opens with Enter", async ({ page }) => {
  await expectLeafActivation(page, async (leaf) => {
    await leaf.focus();
    await page.keyboard.press("Enter");
  });
});

test("size-map file leaf opens with Space", async ({ page }) => {
  await expectLeafActivation(page, async (leaf) => {
    await leaf.focus();
    await page.keyboard.press("Space");
  });
});

test("size-map file leaf opens with a coarse touch", async ({ browser }) => {
  const context = await browser.newContext({
    viewport: { width: 1440, height: 900 },
    hasTouch: true,
  });
  const page = await context.newPage();
  await expectLeafActivation(page, async (leaf) => {
    const box = await leaf.boundingBox();
    expect(box).not.toBeNull();
    await leaf.tap({ position: { x: box.width / 2, y: 10 } });
  });
  await context.close();
});

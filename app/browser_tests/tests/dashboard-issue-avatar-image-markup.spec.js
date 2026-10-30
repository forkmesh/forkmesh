const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const REPO_PAGE_PATH = path.join(PUBLIC, "dashboard", "repo.html");
const DASHBOARD_JS_PATH = path.join(PUBLIC, "dashboard.js");
const DASHBOARD_CSS_PATH = path.join(PUBLIC, "dashboard", "tailwind.css");

const OPENED_AT = 1_795_000_000_000;
const JETT_AVATAR =
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==";

function issueJson() {
  return JSON.stringify({
    number: 8,
    title: "Avatars link to their profiles",
    status: "open",
    createdAt: OPENED_AT,
    events: [
      {
        id: "open-8",
        type: "open",
        ts: OPENED_AT,
        author: "jett-public-key",
        authorName: "jett",
        title: "Avatars link to their profiles",
        body: "A described issue.",
      },
      {
        id: "comment-8",
        type: "comment",
        ts: OPENED_AT + 1,
        author: "mona-public-key",
        authorName: "mona",
        body: "Seen it too.",
      },
    ],
  });
}

async function mockIssuePage(page, requests) {
  await page.addInitScript(() => {
    localStorage.setItem(
      "forkmesh.session",
      JSON.stringify({
        nodeName: "dana",
        sessionToken: "test-session",
        status: "active",
        kind: "user",
        emailVerified: true,
      }),
    );
  });
  await page.route("**/dana/notes/**", (route) =>
    route.fulfill({ path: REPO_PAGE_PATH, contentType: "text/html; charset=utf-8" }),
  );
  await page.route("**/dashboard.js*", (route) =>
    route.fulfill({ path: DASHBOARD_JS_PATH, contentType: "text/javascript; charset=utf-8" }),
  );
  await page.route("**/dashboard/tailwind.css", (route) =>
    route.fulfill({ path: DASHBOARD_CSS_PATH, contentType: "text/css; charset=utf-8" }),
  );
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    requests.push(url.pathname);
    const blobPath = url.searchParams.get("path") || "";
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = {
        ok: true,
        repositories: [
          {
            owner: "dana",
            name: "notes",
            visibility: "public",
            source: "local-node",
            cloneUrl: "https://forkmesh.test/dana/notes",
            rootCommit: "root",
            stateHash: "state",
            updatedAt: "2",
          },
        ],
      };
    } else if (url.pathname === "/api/accounts/jett") {
      body = { ok: true, exists: true, available: false, kind: "user", avatarPng: JETT_AVATAR };
    } else if (url.pathname === "/api/accounts/mona") {
      body = { ok: true, exists: true, available: false, kind: "user", avatarPng: "" };
    } else if (url.pathname === "/api/repo/dana/notes/branches") {
      body = { ok: true, branches: [{ name: "main", commit: "2".repeat(40) }] };
    } else if (url.pathname === "/api/repo/dana/notes/blob") {
      if (blobPath !== ".forkmesh/issues/open/8/issue-8.json") {
        return route.fulfill({
          status: 404,
          contentType: "application/json; charset=utf-8",
          body: JSON.stringify({ ok: false, error: "not_found" }),
        });
      }
      body = { ok: true, content: issueJson() };
    } else if (url.pathname === "/api/repo/dana/notes/tree") {
      body = {
        ok: true,
        commit: "2".repeat(40),
        entries: blobPath === ".forkmesh/issues/open" ? [{ type: "tree", name: "8" }] : [],
      };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
}

test("issue avatars show the account's picture and link to its profile", async ({ page }) => {
  const requests = [];
  await mockIssuePage(page, requests);
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto("/dana/notes/issues/8");

  const detail = page.locator('[data-repo-record-detail="issues"]');
  await expect(detail).toBeVisible({ timeout: 15_000 });

  const jett = detail.locator('[data-repo-issue-opening] a[title="jett"]:visible');
  await expect(jett).toHaveAttribute("href", /\/@jett$/);
  await expect(jett.locator("img")).toHaveAttribute("src", `data:image/png;base64,${JETT_AVATAR}`);

  const mona = detail.locator('[data-repo-issue-comment] a[title="mona"]:visible');
  await expect(mona).toHaveAttribute("href", /\/@mona$/);
  await expect(mona).toHaveAttribute("data-issue-actor-state", "initial");
  await expect(mona).toHaveText("M");

  expect(requests.filter((p) => p === "/api/accounts/jett")).toHaveLength(1);
  expect(requests.filter((p) => p === "/api/accounts/mona")).toHaveLength(1);
  expect(requests).not.toContain("/api/accounts/users");
});

test("a pasted screenshot shows at once, without a dialog, and can be marked up", async ({ page }) => {
  await mockIssuePage(page, []);
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto("/dana/notes/issues/8");
  await expect(page.locator('[data-repo-record-detail="issues"]')).toBeVisible({ timeout: 15_000 });
  await page.locator("[data-repo-issue-add-subissue]").click();
  const body = page.locator("[data-repo-issue-body]");
  await expect(body).toBeVisible();

  const pastedBytes = await body.evaluate(async (textarea) => {
    const canvas = document.createElement("canvas");
    canvas.width = 1600;
    canvas.height = 900;
    const ctx = canvas.getContext("2d");
    ctx.fillStyle = "#fff";
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    for (let row = 0; row < 60; row += 1) {
      ctx.fillStyle = `hsl(${(row * 37) % 360} 70% 35%)`;
      ctx.font = "14px sans-serif";
      ctx.fillText(`Line ${row}: the quick brown fox jumps over the lazy dog ${Math.random()}`.repeat(3), 8, 16 + row * 15);
    }
    const blob = await new Promise((resolve) => canvas.toBlob(resolve, "image/png"));
    const file = new File([blob], "", { type: "image/png" });
    const data = new DataTransfer();
    data.items.add(file);
    textarea.focus();
    textarea.dispatchEvent(new ClipboardEvent("paste", { clipboardData: data, bubbles: true, cancelable: true }));
    return file.size;
  });
  expect(pastedBytes).toBeGreaterThan(40 * 1024);

  const tile = page.locator("[data-repo-issue-attachment]");
  await expect(tile).toHaveCount(1);
  await expect(page.locator('[role="dialog"][aria-label="Resize image"]')).toHaveCount(0);
  await expect(tile.locator("img")).toBeVisible();
  await expect(body).toHaveValue(/!\[screenshot\.png\]\(forkmesh-pending-image:/);
  const firstSrc = await tile.locator("img").getAttribute("src");
  expect(firstSrc).toMatch(/^data:image\/jpeg;base64,/);
  expect(Math.floor((firstSrc.length - firstSrc.indexOf(",") - 1) * 0.75)).toBeLessThanOrEqual(40 * 1024);

  const markupButton = tile.getByRole("button", { name: "Mark up", exact: true });
  const hoverActions = markupButton.locator("..");
  await page.mouse.move(0, 0);
  await expect(hoverActions).toHaveCSS("opacity", "0");
  await tile.hover();
  await expect(hoverActions).toHaveCSS("opacity", "1");
  await markupButton.click();

  const canvas = page.locator("[data-markup-canvas]");
  await expect(canvas).toBeVisible();
  await expect(page.locator("[data-markup-status]")).toContainText("press Done");
  await page.locator('[data-markup-tool="box"]').click();
  const box = await canvas.boundingBox();
  await page.mouse.move(box.x + box.width * 0.2, box.y + box.height * 0.2);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width * 0.6, box.y + box.height * 0.5, { steps: 5 });
  await page.mouse.up();
  const redPixel = await canvas.evaluate((element, x) => {
    const [r, g, b] = element.getContext("2d").getImageData(Math.round(x), Math.round(element.height * 0.35), 1, 1).data;
    return r > 180 && g < 120 && b < 120;
  }, 1600 * 0.2);
  expect(redPixel).toBe(true);
  await page.locator("[data-markup-done]").click();

  await expect(canvas).toHaveCount(0);
  await expect(tile).toContainText("Marked up");
  const markedSrc = await tile.locator("img").getAttribute("src");
  expect(markedSrc).not.toBe(firstSrc);
  expect(Math.floor((markedSrc.length - markedSrc.indexOf(",") - 1) * 0.75)).toBeLessThanOrEqual(40 * 1024);

  await tile.hover();
  await tile.getByRole("button", { name: "Mark up", exact: true }).click();
  await expect(page.locator("[data-markup-undo]")).toBeEnabled();
  await page.locator("[data-image-markup]").press("Escape");
  await expect(page.locator("[data-markup-canvas]")).toHaveCount(0);
  expect(await tile.locator("img").getAttribute("src")).toBe(markedSrc);

  await tile.hover();
  await tile.getByRole("button", { name: /Remove/ }).click();
  await expect(tile).toHaveCount(0);
  await expect(body).not.toHaveValue(/forkmesh-pending-image/);
});

const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const PAGE_PATH = path.join(PUBLIC, "start-discourse-mirror.html");
const SCRIPT_PATH = path.join(PUBLIC, "start-discourse-mirror.js");
const SHOTS = process.env.FORKMESH_FORUM_SHOTS || "";
const ORIGIN = "https://forum.example.org";

const REPORT = {
  ok: true, origin: ORIGIN, reachable: true, discourse: true, public: true, robotsAllow: true,
  title: "Example Forum", description: "", categories: 12, topics: 3400, posts: 136000,
  estimate: { requests: 6900, hours: 1.9, runs: 9 }, ready: true,
};

async function stage(page, { who = { status: 200, body: { ok: true, actor: "ada", admin: true } }, query = "" } = {}) {
  const calls = [];
  await page.route("**/start-discourse-mirror*", (route) => {
    const url = new URL(route.request().url());
    if (url.pathname === "/start-discourse-mirror.js") {
      return route.fulfill({ path: SCRIPT_PATH, contentType: "text/javascript; charset=utf-8" });
    }
    return route.fulfill({ path: PAGE_PATH, contentType: "text/html; charset=utf-8" });
  });
  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const body = request.postData() ? JSON.parse(request.postData()) : null;
    calls.push({ method: request.method(), path: url.pathname, search: url.search, body });
    if (url.pathname === "/api/forum-mirrors" && request.method() === "GET") {
      if (!url.searchParams.get("origin")) return route.fulfill({ status: who.status, json: who.body });
      return route.fulfill({ json: REPORT });
    }
    if (url.pathname === "/api/repositories") {
      return route.fulfill({ json: { ok: true, repositories: [
        { owner: "ada", name: "notes", defaultBranch: "trunk" },
        { owner: "someone", name: "theirs" },
      ] } });
    }
    if (url.pathname === "/api/hosted-repositories") {
      return route.fulfill({ json: { ok: true, repository: { owner: "ada", name: body.name } } });
    }
    if (url.pathname === "/api/forum-mirrors" && request.method() === "POST") {
      return route.fulfill({ json: { ok: true, commit: "c".repeat(40), dispatched: true, runId: 7000,
        estimate: REPORT.estimate, origin: ORIGIN } });
    }
    return route.fulfill({ status: 404, json: { error: "not_found" } });
  });
  await page.goto(`/start-discourse-mirror${query}`);
  return calls;
}

test("an admin checks a forum, creates a repository and starts the mirror", async ({ page }) => {
  await page.setViewportSize({ width: 1100, height: 1300 });
  const calls = await stage(page, { query: `?forum=${encodeURIComponent(ORIGIN)}` });
  await expect(page.locator("[data-dm-form]")).toBeVisible();
  await expect(page.locator("[data-dm-report]")).toContainText("Ready to mirror: Example Forum");
  await expect(page.locator("[data-dm-report]")).toContainText("3,400");
  await expect(page.locator("[data-dm-new-name]")).toHaveValue("example-forum");
  await expect(page.locator("[data-dm-existing-select] option")).toHaveCount(1);
  const start = page.locator("[data-dm-start]");
  await expect(start).toBeDisabled();
  await page.locator("[data-dm-agree]").check();
  await expect(start).toBeEnabled();
  if (SHOTS) await page.screenshot({ path: path.join(SHOTS, "start-discourse-mirror.png"), fullPage: true });
  await start.click();
  await expect(page.locator("[data-dm-done]")).toBeVisible();
  await expect(page.locator("[data-dm-done-text]")).toContainText("the first run has started");
  await expect(page.locator("[data-dm-forum-link]")).toHaveAttribute("href", "/ada/example-forum/forum");
  await expect(page.locator("[data-dm-actions-link]")).toHaveAttribute("href", "/ada/example-forum/actions");
  const created = calls.find((call) => call.path === "/api/hosted-repositories");
  expect(created.body).toMatchObject({ name: "example-forum", visibility: "public", readme: true });
  const started = calls.find((call) => call.path === "/api/forum-mirrors" && call.method === "POST");
  expect(started.body).toMatchObject({ owner: "ada", repo: "example-forum", branch: "main", origin: ORIGIN,
    uploads: true, agree: true });
  if (SHOTS) await page.screenshot({ path: path.join(SHOTS, "start-discourse-mirror-done.png"), fullPage: true });
});

test("an existing repository keeps its own default branch", async ({ page }) => {
  const calls = await stage(page);
  await page.locator("[data-dm-origin]").fill(ORIGIN);
  await page.locator("[data-dm-check]").click();
  await expect(page.locator("[data-dm-report]")).toContainText("Ready to mirror");
  await page.locator('[data-dm-repo-kind][value="existing"]').check();
  await page.locator("[data-dm-uploads]").uncheck();
  await page.locator("[data-dm-agree]").check();
  await page.locator("[data-dm-start]").click();
  await expect(page.locator("[data-dm-done]")).toBeVisible();
  const started = calls.find((call) => call.path === "/api/forum-mirrors" && call.method === "POST");
  expect(started.body).toMatchObject({ owner: "ada", repo: "notes", branch: "trunk", uploads: false });
  expect(calls.some((call) => call.path === "/api/hosted-repositories")).toBe(false);
});

test("someone who is not an admin is sent to start an instance of their own", async ({ page }) => {
  await stage(page, { who: { status: 403, body: { error: "admin_required" } }, query: `?forum=${encodeURIComponent(ORIGIN)}` });
  await expect(page.locator("[data-dm-not-admin]")).toBeVisible();
  await expect(page.locator("[data-dm-form]")).toBeHidden();
  await expect(page.locator("[data-dm-launch-link]")).toHaveAttribute(
    "href", `https://forkmesh.com/start?forum=${encodeURIComponent(ORIGIN)}`);
});

test("a signed-out visitor is asked to sign in and comes back", async ({ page }) => {
  await stage(page, { who: { status: 401, body: { error: "invalid_session" } }, query: "?forum=x" });
  await expect(page.locator("[data-dm-signin]")).toBeVisible();
  await expect(page.locator("[data-dm-signin-link]")).toHaveAttribute(
    "href", `/login?next=${encodeURIComponent("/start-discourse-mirror?forum=x")}`);
});

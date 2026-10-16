const { test, expect } = require("@playwright/test");
const path = require("node:path");

const TASKS_PAGE_PATH = path.resolve(
  __dirname, "..", "..", "public", "dashboard", "tasks", "index.html",
);
const SHOT_DIR = process.env.FORKMESH_TASK_PROGRESS_SHOTS || "";

const NOW = Date.now();

function task(id, overrides) {
  return {
    id,
    title: "Task",
    details: "",
    status: "idle",
    completedAt: 0,
    assignee: "alice",
    assigneeKind: "user",
    createdBy: "jett",
    department: "engineering",
    team: "",
    destination: "department",
    priority: 50,
    progress: 0,
    repository: "",
    elapsedMs: 0,
    startedAt: 0,
    nextCheckinAt: 0,
    createdAt: NOW - 7_200_000,
    updatedAt: NOW - 60_000,
    qa: { status: "unknown", reviewer: "", reviewedAt: 0, requestedAt: 0 },
    lastCheckin: null,
    attachments: [],
    completionHistory: [],
    replies: [],
    history: [],
    ...overrides,
  };
}

const MINE = task("aaaaaaaabbbbccccddddeeeeffff0001", {
  title: "[In progress] Org-owner push to the cores", status: "active", priority: 5, progress: 40,
  startedAt: NOW - 60_000, nextCheckinAt: NOW + 300_000,
});
const OTHERS = task("aaaaaaaabbbbccccddddeeeeffff0002", {
  title: "Native iOS", assignee: "wendy", priority: 10, progress: 75,
});
const DONE = task("aaaaaaaabbbbccccddddeeeeffff0003", {
  title: "Caddy upgrade", status: "done", completedAt: NOW - 3_600_000, priority: 20, progress: 60,
});
const RED_PIXEL = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";
const FRESH = task("aaaaaaaabbbbccccddddeeeeffff0004", {
  title: "Link-grant fix", assignee: "wendy", priority: 30,
  attachments: [
    { id: "aaaaaaaabbbbccccddddeeeeffff00a1", name: "screen.png", mime: "image/png", size: 68, url: "/api/tasks/aaaaaaaabbbbccccddddeeeeffff0004/attachments/aaaaaaaabbbbccccddddeeeeffff00a1", thumbnail: RED_PIXEL },
    { id: "aaaaaaaabbbbccccddddeeeeffff00a2", name: "spinner.gif", mime: "image/gif", size: 900, url: "/api/tasks/aaaaaaaabbbbccccddddeeeeffff0004/attachments/aaaaaaaabbbbccccddddeeeeffff00a2" },
  ],
});

async function openTasks(page, { patched = [], canManage = false, colorScheme = "dark" } = {}) {
  await page.emulateMedia({ colorScheme });
  await page.addInitScript(() => {
    window.ForkMeshDashboard = {
      _registrations: [["tasks", ({ builtinPage }) => builtinPage("tasks")]],
    };
  });
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alice",
      email: "alice@example.test",
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
  await page.route("**/dashboard/tasks", (route) => route.fulfill({
    path: TASKS_PAGE_PATH,
    contentType: "text/html; charset=utf-8",
  }));
  const tasks = new Map([MINE, OTHERS, DONE, FRESH].map((item) => [item.id, { ...item }]));
  await page.route("**/api/**", async (route) => {
    const url = new URL(route.request().url());
    const request = route.request();
    let body = { ok: true };
    if (url.pathname === "/api/tasks") {
      body = { ok: true, actor: "alice", canManage, members: ["alice", "jett", "wendy"], tasks: [...tasks.values()] };
    } else {
      const id = url.pathname.split("/")[3];
      const current = tasks.get(id);
      if (current && request.method() === "PATCH") {
        const sent = JSON.parse(request.postData() || "{}");
        patched.push({ id, ...sent });
        current.progress = sent.progress;
      }
      if (current) body = { ok: true, task: current };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.goto("/dashboard/tasks");
  await expect(page.locator("[data-tasks-list] .task-row")).toHaveCount(3);
}

function rowBar(page, id) {
  return page.locator(`[data-task-select="${id}"] [role="progressbar"]`);
}

test("every row carries an accessible progress bar and done tasks are full", async ({ page }) => {
  await openTasks(page);
  await page.locator('[data-tasks-filter="all"]').click();
  await expect(page.locator("[data-tasks-list] .task-row")).toHaveCount(4);
  await expect(rowBar(page, MINE.id)).toHaveAttribute("aria-valuenow", "40");
  await expect(rowBar(page, OTHERS.id)).toHaveAttribute("aria-valuenow", "75");
  await expect(rowBar(page, FRESH.id)).toHaveAttribute("aria-valuenow", "0");
  await expect(rowBar(page, DONE.id)).toHaveAttribute("aria-valuenow", "100");
  await expect(rowBar(page, DONE.id)).toHaveAttribute("aria-valuetext", "100% complete");
  await expect(rowBar(page, MINE.id)).toHaveAttribute("data-state", "active");
  await expect(rowBar(page, DONE.id)).toHaveAttribute("data-state", "done");
  await expect(page.locator('[data-task-group="active"] .task-row')).toHaveCount(1);
  await expect(page.locator('[data-task-group="ready"] .task-row')).toHaveCount(2);
  await expect(page.locator('[data-task-group="done"] .task-row')).toHaveCount(1);
  if (SHOT_DIR) await page.screenshot({ path: path.join(SHOT_DIR, "tasks-progress-dark.png"), fullPage: false });
});

test("rows show the images attached to each task", async ({ page }) => {
  const imageRequests = [];
  page.on("request", (request) => {
    if (request.url().includes("/attachments/")) imageRequests.push(request.url());
  });
  await openTasks(page);
  const thumbs = page.locator(`[data-task-select="${FRESH.id}"] .task-row-thumb`);
  await expect(thumbs).toHaveCount(2);
  await expect(thumbs.nth(0).locator("img")).toHaveAttribute("src", RED_PIXEL);
  await expect(thumbs.nth(0)).toHaveAttribute("title", "screen.png");
  await expect(thumbs.nth(1).locator("img")).toHaveCount(0);
  await expect(page.locator(`[data-task-select="${FRESH.id}"] .task-row-thumbs`)).toHaveAttribute("aria-label", "2 images attached");
  await expect(page.locator(`[data-task-select="${OTHERS.id}"] .task-row-thumbs`)).toHaveCount(0);
  const box = await thumbs.nth(0).boundingBox();
  const rem = await page.evaluate(() => parseFloat(getComputedStyle(document.documentElement).fontSize));
  expect(Math.abs(box.width - 1.5 * rem)).toBeLessThan(1);
  expect(Math.abs(box.height - box.width)).toBeLessThan(1);
  expect(imageRequests).toEqual([]);
  if (SHOT_DIR) await page.locator("[data-tasks-list]").screenshot({ path: path.join(SHOT_DIR, "tasks-row-thumbnails.png") });
});

test("an assignee sets progress from the detail slider", async ({ page }) => {
  const patched = [];
  await openTasks(page, { patched });
  await page.locator(`[data-task-select="${MINE.id}"]`).click();
  const detail = page.locator("[data-tasks-detail]");
  await expect(detail.locator('[role="progressbar"]')).toHaveAttribute("aria-valuenow", "40");
  const range = detail.locator("[data-task-progress-range]");
  await range.fill("65");
  await expect(detail.locator("[data-task-progress-number]")).toHaveValue("65");
  await detail.locator('[data-task-progress-form] button[type="submit"]').click();
  await expect.poll(() => patched.length).toBe(1);
  expect(patched[0]).toEqual({ id: MINE.id, progress: 65 });
  await expect(rowBar(page, MINE.id)).toHaveAttribute("aria-valuenow", "65");
  await expect(page.locator('[data-task-notice="progress"]')).toContainText("65%");
});

test("a member who is not an editor sees the bar without the slider", async ({ page }) => {
  await openTasks(page);
  await page.locator(`[data-task-select="${OTHERS.id}"]`).click();
  const detail = page.locator("[data-tasks-detail]");
  await expect(detail.locator('[role="progressbar"]')).toHaveAttribute("aria-valuenow", "75");
  await expect(detail.locator("[data-task-progress-form]")).toHaveCount(0);
});

test("managers can edit any open task and the bars fit a phone in light mode", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await openTasks(page, { canManage: true, colorScheme: "light" });
  await page.locator(`[data-task-select="${OTHERS.id}"]`).click();
  await expect(page.locator("[data-tasks-detail] [data-task-progress-form]")).toHaveCount(1);
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
  expect(overflow).toBeLessThanOrEqual(0);
  await expect(rowBar(page, MINE.id)).toBeVisible();
  await expect(page.locator("[data-tasks-detail] [role=\"progressbar\"]")).toBeVisible();
  if (SHOT_DIR) await page.screenshot({ path: path.join(SHOT_DIR, "tasks-progress-mobile-light.png"), fullPage: false });
});

test("status tabs are icons with counts and member avatars filter the board", async ({ page }) => {
  await openTasks(page);
  const count = (key) => page.locator(`[data-tasks-filter-count="${key}"]`);
  await expect(count("open")).toHaveText("3");
  await expect(count("active")).toHaveText("1");
  await expect(count("mine")).toHaveText("1");
  await expect(count("done")).toHaveText("1");
  await expect(count("all")).toHaveText("4");
  await expect(count("trash")).toBeHidden();
  await expect(page.locator('[data-tasks-filter="open"]')).toHaveAttribute("aria-label", "Open: 3");

  const person = (value) => page.locator(`[data-tasks-person="${value}"]`);
  await expect(page.locator("[data-tasks-person]")).toHaveCount(6);
  await expect(page.locator("[data-tasks-person]").nth(1)).toHaveAttribute("data-tasks-person", "user:alice");
  await expect(person("all").locator(".task-person-count")).toHaveText("3");
  await expect(person("user:alice").locator(".task-person-count")).toHaveText("1");
  await expect(person("user:jett").locator(".task-person-count")).toHaveText("0");
  await expect(person("user:wendy").locator(".task-person-count")).toHaveText("2");
  await expect(person("user:wendy").locator("[data-task-assignee-avatar]")).toHaveAttribute("data-task-assignee-avatar", "wendy");

  await person("user:wendy").click();
  await expect(person("user:wendy")).toHaveAttribute("aria-pressed", "true");
  await expect(page.locator("[data-tasks-list] .task-row")).toHaveCount(2);
  await expect(count("open")).toHaveText("2");
  await expect(count("all")).toHaveText("2");
  await expect(count("mine")).toHaveText("1");
  if (SHOT_DIR) await page.screenshot({ path: path.join(SHOT_DIR, "tasks-people-dark.png"), fullPage: false });

  await person("user:wendy").click();
  await expect(person("all")).toHaveAttribute("aria-pressed", "true");
  await expect(page.locator("[data-tasks-list] .task-row")).toHaveCount(3);

  await person("user:jett").click();
  await expect(page.locator("[data-tasks-list] .task-row")).toHaveCount(0);
  await page.locator('[data-tasks-filter="mine"]').click();
  await expect(person("all")).toHaveAttribute("aria-pressed", "true");
  await expect(page.locator("[data-tasks-list] .task-row")).toHaveCount(1);
});

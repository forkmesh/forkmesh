// Comments, @mentions, the check-in button and one task's activity timeline,
// driven on the real Tasks page: the detail pane merges lifecycle events with
// comment bodies, "@" offers real teammates, a posted comment names who it
// pinged, and the check-in button reaches the relay with the chosen state.
const { test, expect } = require("@playwright/test");
const path = require("node:path");

const TASKS_PAGE_PATH = path.resolve(
  __dirname, "..", "..", "public", "dashboard", "tasks", "index.html",
);

const NOW = Date.now();

const TASK = {
  id: "aaaaaaaabbbbccccddddeeeeffff0001",
  title: "Fix the login redirect",
  details: "Reported by @wendy after the release.",
  status: "active",
  completedAt: 0,
  assignee: "alice",
  assigneeKind: "user",
  createdBy: "jett",
  department: "engineering",
  team: "frontend",
  destination: "department",
  priority: 20,
  repository: "forkmesh/site",
  elapsedMs: 60_000,
  startedAt: NOW - 60_000,
  nextCheckinAt: NOW + 300_000,
  createdAt: NOW - 7_200_000,
  updatedAt: NOW - 60_000,
  qa: { status: "unknown", reviewer: "", reviewedAt: 0, requestedAt: 0 },
  lastCheckin: null,
  attachments: [],
  completionHistory: [],
  replyCount: 1,
};

const DETAIL = {
  ...TASK,
  history: [
    { id: "e1", kind: "created", actor: "jett", summary: TASK.title, mentions: [], createdAt: NOW - 7_200_000 },
    { id: "e2", kind: "started", actor: "alice", summary: "", mentions: [], createdAt: NOW - 60_000 },
    { id: "e3", kind: "replied", actor: "wendy", summary: "any luck?", mentions: [], createdAt: NOW - 30_000 },
  ],
  replies: [
    { id: "r1", author: "wendy", body: "any luck? @alice", context: {}, createdAt: NOW - 30_000 },
  ],
};

async function openTasks(page, { posted = [] } = {}) {
  // Tasks is a packaged page: its Marketplace entrypoint (only copied into the
  // deploy stage) is what registers the built-in initializer. Queue the same
  // registration the package makes, so the bundle boots the real page.
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
  await page.route("**/api/**", async (route) => {
    const url = new URL(route.request().url());
    let body = { ok: true };
    if (url.pathname === "/api/tasks") {
      body = { ok: true, actor: "alice", canManage: false, members: ["alice", "jett", "wendy"], tasks: [TASK] };
    } else if (url.pathname === `/api/tasks/${TASK.id}/replies`) {
      const sent = JSON.parse(route.request().postData() || "{}");
      posted.push({ kind: "reply", ...sent });
      body = {
        ok: true,
        task: {
          ...DETAIL,
          replyCount: DETAIL.replyCount + 1,
          replies: [
            ...DETAIL.replies,
            { id: "r2", author: "alice", body: sent.body, context: {}, createdAt: NOW },
          ],
          history: [
            ...DETAIL.history,
            { id: "e4", kind: "replied", actor: "alice", summary: sent.body, mentions: [], createdAt: NOW },
          ],
        },
      };
    } else if (url.pathname === `/api/tasks/${TASK.id}/checkin`) {
      const sent = JSON.parse(route.request().postData() || "{}");
      posted.push({ kind: "checkin", ...sent });
      body = {
        ok: true,
        task: {
          ...DETAIL,
          lastCheckin: { state: sent.state, note: sent.note || "", at: NOW },
          history: [
            ...DETAIL.history,
            { id: "e5", kind: "checkin", actor: "alice", summary: "blocked", mentions: [], createdAt: NOW },
          ],
        },
      };
    } else if (url.pathname === `/api/tasks/${TASK.id}`) {
      body = { ok: true, task: DETAIL };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.goto("/dashboard/tasks");
}

test("the detail pane merges lifecycle events with comment bodies", async ({ page }) => {
  await openTasks(page);
  const timeline = page.locator("[data-tasks-detail] .task-timeline");
  await expect(timeline).toContainText("created this task");
  await expect(timeline).toContainText("started work");
  // The "replied" event and its reply row share a timestamp: one comment, not two.
  await expect(timeline.locator('[data-kind="comment"]')).toHaveCount(1);
  await expect(timeline.locator('[data-kind="comment"]')).toContainText("any luck?");
  await expect(timeline.locator('.task-mention[data-me="true"]')).toHaveText("@alice");
});

test("typing @ suggests teammates and the posted comment names who it pinged", async ({ page }) => {
  const posted = [];
  await openTasks(page, { posted });
  const composer = page.locator("[data-task-reply-input]");
  await composer.click();
  await composer.pressSequentially("@we");
  await expect(page.locator('[data-task-mention-pick="wendy"]')).toBeVisible();
  await composer.press("Enter");
  await expect(composer).toHaveValue("@wendy ");
  await composer.pressSequentially("please look");
  await page.locator('[data-task-reply-form] button[type="submit"]').click();
  await expect.poll(() => posted.length).toBe(1);
  expect(posted[0]).toMatchObject({ kind: "reply", body: "@wendy please look" });
  await expect(page.locator('[data-task-notice="activity"]')).toContainText("@wendy");
  await expect(page.locator('.task-timeline [data-kind="comment"]')).toHaveCount(2);
});

test("the assignee can check in and the team is told", async ({ page }) => {
  const posted = [];
  await openTasks(page, { posted });
  await page.locator('[data-task-action="checkin"]').click();
  const note = page.locator("[data-task-checkin-note]");
  await expect(note).toBeFocused();
  await note.fill("waiting on review");
  await page.locator('[data-task-checkin-state="blocked"]').click();
  await expect.poll(() => posted.length).toBe(1);
  expect(posted[0]).toMatchObject({ kind: "checkin", state: "blocked", note: "waiting on review" });
  await expect(page.locator('[data-task-notice="actions"]')).toContainText("Check-in sent");
  await expect(page.locator('.task-timeline .task-checkin-chip[data-state="blocked"]')).toBeVisible();
  await expect(page.locator("[data-task-checkin-note]")).toHaveCount(0);
});

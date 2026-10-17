const { test, expect } = require("@playwright/test");
const path = require("node:path");

const TASKS_PAGE_PATH = path.resolve(
  __dirname, "..", "..", "public", "dashboard", "tasks", "index.html",
);

const NOW = Date.now();

const TASK = {
  id: "aaaaaaaabbbbccccddddeeeeffff0002",
  title: "Tidy the release notes",
  details: "Group the fixes by area.",
  status: "ready",
  completedAt: 0,
  assignee: "",
  assigneeKind: "unassigned",
  createdBy: "jett",
  department: "engineering",
  team: "",
  destination: "department",
  priority: 40,
  repository: "forkmesh/site",
  createdAt: NOW - 7_200_000,
  updatedAt: NOW - 60_000,
  qa: { status: "unknown", reviewer: "", reviewedAt: 0, requestedAt: 0 },
  attachments: [],
  replyCount: 0,
  history: [],
  replies: [],
};

async function openTasks(page, { canManage = true, sent = [] } = {}) {
  let task = { ...TASK };
  await page.addInitScript(() => {
    window.ForkMeshDashboard = {
      _registrations: [["tasks", ({ builtinPage }) => builtinPage("tasks")]],
    };
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
    const method = route.request().method();
    let body = { ok: true };
    if (url.pathname === "/api/tasks") {
      body = {
        ok: true, actor: "alice", canManage,
        members: ["alice", "jett"], departments: ["general", "engineering", "design"],
        teams: ["frontend"], tasks: [task],
      };
    } else if (url.pathname === `/api/tasks/${TASK.id}` && method === "PATCH") {
      const change = JSON.parse(route.request().postData() || "{}");
      sent.push({ method, path: url.pathname, ...change });
      task = { ...task, ...change, updatedAt: Date.now() };
      body = { ok: true, task };
    } else if (url.pathname.startsWith(`/api/tasks/${TASK.id}/`) && method === "POST") {
      const action = url.pathname.split("/").pop();
      sent.push({ method, path: url.pathname });
      if (action === "start") task = { ...task, status: "active", assigneeKind: "user", assignee: "alice" };
      body = { ok: true, task };
    } else if (url.pathname === `/api/tasks/${TASK.id}`) {
      body = { ok: true, task };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.goto("/dashboard/tasks");
}

test("a manager edits task properties in place from the side column", async ({ page }) => {
  const sent = [];
  await openTasks(page, { sent });
  const aside = page.locator(".task-detail-aside");
  const quick = page.locator(".task-detail-head [data-task-reply-form] [data-task-reply-input]");
  await expect(quick).toBeVisible();
  if (process.env.FORKMESH_TASK_DETAIL_SHOT) {
    await page.setViewportSize({ width: 1440, height: 900 });
    await page.screenshot({ path: process.env.FORKMESH_TASK_DETAIL_SHOT });
  }

  await aside.locator('[data-task-prop="priority"]').fill("7");
  await aside.locator('[data-task-prop="priority"]').press("Enter");
  await expect.poll(() => sent.find((item) => item.priority !== undefined)).toMatchObject({ method: "PATCH", priority: 7 });

  await aside.locator('[data-task-prop="assignee"]').selectOption("user:jett");
  await expect.poll(() => sent.find((item) => item.assigneeKind)).toMatchObject({ assigneeKind: "user", assignee: "jett" });

  await aside.locator('[data-task-prop="department"]').selectOption("design");
  await expect.poll(() => sent.find((item) => item.department)).toMatchObject({ department: "design" });

  await aside.locator('[data-task-prop="repository"]').fill("forkmesh/docs");
  await aside.locator('[data-task-prop="repository"]').press("Enter");
  await expect.poll(() => sent.find((item) => item.repository)).toMatchObject({ repository: "forkmesh/docs" });
  await expect(aside.locator('[data-task-prop="repository"]')).toHaveValue("forkmesh/docs");
});

test("a member without manage rights sees the properties as text", async ({ page }) => {
  await openTasks(page, { canManage: false });
  const aside = page.locator(".task-detail-aside");
  await expect(aside).toContainText("Unassigned");
  await expect(aside.locator('[data-task-prop="assignee"]')).toHaveCount(0);
  await expect(aside.locator('[data-task-prop="repository"]')).toHaveCount(0);
  await expect(aside.locator('[data-task-prop="priority"]')).toHaveCount(0);
});

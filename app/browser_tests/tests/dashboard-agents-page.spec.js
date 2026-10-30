const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public", "dashboard");
const AGENTS_PAGE_PATH = path.join(PUBLIC, "agents", "index.html");
const REPO_PAGE_PATH = path.join(PUBLIC, "repo.html");

const REPOSITORIES = [{
  owner: "alpha",
  name: "engine",
  description: "Physics core",
  visibility: "public",
  source: "remote-clone",
  cloneUrl: "https://forkmesh.test/alpha/engine",
  rootCommit: "root-0",
  stateHash: "state-0",
  updatedAt: "1",
}];

const NOW = Date.now();

const SESSIONS = [
  {
    id: "session-running",
    org: "alpha",
    repo: "engine",
    scope: "owner",
    status: "running",
    displayStatus: "running",
    provider: "claude-code",
    title: "Rebuild the solver",
    targetNode: "mirror35",
    createdBy: "alpha",
    createdAt: NOW - 600000,
    updatedAt: NOW - 1000,
    agentInfo: { model: "opus" },
    diagnostic: { code: "running", level: "active", message: "The mirror is running this agent session." },
    history: [{ role: "user", author: "alpha", text: "Rebuild the solver.", at: NOW - 600000, state: "queued" }],
  },
  {
    id: "session-done",
    org: "acme",
    repo: "web",
    scope: "organization",
    status: "completed",
    displayStatus: "completed",
    provider: "codex",
    title: "Tidy the footer",
    targetNode: "acme-runner",
    createdBy: "alpha",
    createdAt: NOW - 7200000,
    updatedAt: NOW - 3600000,
    agentInfo: { model: "sol" },
    diagnostic: { code: "completed", level: "ok", message: "The mirror completed this agent session." },
    history: [],
  },
];

const DESKTOP = [{ id: "12", owner: "alpha-laptop", repo: "notes", node: "alpha-laptop", reportedAt: NOW - 120000 }];

async function stubDashboard(page, pagePath, route, api = null) {
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alpha",
      email: "alpha@example.test",
      emailVerified: true,
      sessionToken: "test-session",
      status: "active",
    }));
  });
  await page.route("https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js",
    (r) => r.fulfill({ contentType: "text/javascript", body: "window.lucide = { createIcons() {} };" }));
  await page.route(route, (r) => r.fulfill({ path: pagePath, contentType: "text/html; charset=utf-8" }));
  const reads = [];
  await page.route("**/api/**", async (r) => {
    const url = new URL(r.request().url());
    reads.push(url.pathname);
    let body = { ok: true };
    const custom = api ? await api(r.request(), url) : null;
    if (custom) {
      return r.fulfill({ status: custom.status || 200, contentType: "application/json; charset=utf-8", body: JSON.stringify(custom.body) });
    }
    if (url.pathname === "/api/repositories") {
      body = { ok: true, repositories: REPOSITORIES };
    } else if (url.pathname === "/api/account/agent-sessions") {
      body = { ok: true, account: "alpha", scope: "owner", activeCount: 1, finishedBefore: 0, sessions: SESSIONS };
    } else if (url.pathname === "/api/account/desktop-agents") {
      body = { ok: true, account: "alpha", agents: DESKTOP, truncated: false };
    } else if (/\/agent-(bots|sessions)$/.test(url.pathname)) {
      body = {
        ok: true, memberRole: "owner", scope: "owner", canQueueAgent: true,
        canViewAgentSessions: true, canDeleteAgentSessions: true, sessions: SESSIONS.slice(0, 1),
      };
    }
    return r.fulfill({ status: 200, contentType: "application/json; charset=utf-8", body: JSON.stringify(body) });
  });
  return reads;
}

test("the Agents page lists cloud, organization and desktop sessions with filters", async ({ page }) => {
  const reads = await stubDashboard(page, AGENTS_PAGE_PATH, "**/dashboard/agents*");
  await page.goto("/dashboard/agents");
  const rows = page.locator("[data-agents-row]");
  await expect(rows).toHaveCount(3);
  await expect(rows.nth(0)).toHaveAttribute("data-agents-row", "cloud:session-running");
  await expect(rows.nth(1)).toHaveAttribute("data-agents-kind", "desktop");
  expect(reads).toContain("/api/account/agent-sessions");
  expect(reads).toContain("/api/account/desktop-agents");

  await expect(page.locator('a[data-nav="agents"]')).toHaveAttribute("aria-current", "page");
  await expect(page.locator("[data-agents-count]")).toHaveText("3 sessions · 1 working");

  const running = page.locator('[data-agents-row="cloud:session-running"]');
  await expect(running).toHaveAttribute("href", "/alpha/engine/agents?session=session-running");
  await expect(running).toContainText("Rebuild the solver");
  await expect(running).toContainText("alpha/engine");
  await expect(running).toContainText("Claude Code · opus");
  await expect(running).toContainText("mirror35");
  await expect(running.locator('[data-agents-status="running"]')).toHaveText("Working");
  await expect(running.locator(".animate-spin")).toHaveCount(1);

  const org = page.locator('[data-agents-row="cloud:session-done"]');
  await expect(org).toHaveAttribute("href", "/acme/web/agents?session=session-done");
  await expect(org).toContainText("Organization run");

  const desktop = page.locator('[data-agents-row="desktop:alpha-laptop/notes:12"]');
  await expect(desktop).toHaveAttribute("data-agents-kind", "desktop");
  await expect(desktop).toHaveAttribute("href", "/alpha-laptop/notes/agents");
  await expect(desktop).toContainText("alpha-laptop/notes");
  await expect(desktop).toContainText("On desktop");

  await page.selectOption('[data-agents-filter="status"]', "active");
  await expect(rows).toHaveCount(1);
  await expect(page).toHaveURL(/\/dashboard\/agents\?status=active$/);
  await page.selectOption('[data-agents-filter="status"]', "");
  await page.selectOption('[data-agents-filter="node"]', "acme-runner");
  await expect(rows).toHaveCount(1);
  await expect(rows.nth(0)).toContainText("Tidy the footer");
  await page.selectOption('[data-agents-filter="node"]', "");
  await page.selectOption('[data-agents-filter="repo"]', "alpha-laptop/notes");
  await expect(rows).toHaveCount(1);
  await expect(rows.nth(0)).toHaveAttribute("data-agents-kind", "desktop");
});

test("a linked session opens straight into its conversation on the repository Agents tab", async ({ page }) => {
  await stubDashboard(page, REPO_PAGE_PATH, "**/alpha/engine/agents*");
  await page.goto("/alpha/engine/agents?session=session-running");
  await expect(page.locator('[data-org-agent-detail="session-running"]')).toHaveCount(1);
  await expect(page.locator('[data-org-agent-detail="session-running"]')).toContainText("Rebuild the solver.");
  await expect(page).toHaveURL(/\/alpha\/engine\/agents$/);
});

const ACCOUNT_A = "a".repeat(32);
const ACCOUNT_B = "b".repeat(32);

function runnersPayload(pending) {
  return {
    ok: true,
    runners: [
      {
        name: "fmnext-r1",
        scope: "pool",
        status: "active",
        availability: {
          "claude-code": {
            binaryFound: true,
            loginState: "available",
            accounts: [
              { id: ACCOUNT_A, label: "one@example.com", plan: "max", active: true,
                usage: { windows: [{ id: "five_hour", label: "5-hour session", usedPercent: 42.4 }] } },
              { id: ACCOUNT_B, label: "", email: "two@example.com", active: false, usage: null },
            ],
          },
          codex: { binaryFound: true, loginState: "missing", accounts: [] },
        },
        accountRequests: pending
          ? [{ id: "c".repeat(32), provider: "claude-code", action: "activate", account: ACCOUNT_B, state: "pending" }]
          : [],
      },
      { name: "old-runner", status: "revoked", availability: { "claude-code": { binaryFound: true, accounts: [] } } },
    ],
  };
}

test("the Agents page switches the account a runner uses", async ({ page }) => {
  const posts = [];
  await stubDashboard(page, AGENTS_PAGE_PATH, "**/dashboard/agents*", async (request, url) => {
    if (url.pathname === "/api/runners") return { body: runnersPayload(posts.length > 0) };
    if (url.pathname === "/api/runners/fmnext-r1/accounts" && request.method() === "POST") {
      posts.push(request.postDataJSON());
      return { status: 202, body: { ok: true, id: "c".repeat(32) } };
    }
    return null;
  });
  await page.goto("/dashboard/agents");
  const picker = page.locator("[data-runner-accounts-picker]");
  await expect(picker).toBeVisible();
  await expect(page.locator("[data-runner-accounts-picker-row]")).toHaveCount(2);
  const select = page.locator('[data-runner-account-select="fmnext-r1:claude-code"]');
  await expect(select).toHaveValue(ACCOUNT_A);
  await expect(select.locator("option").nth(0)).toHaveText("one@example.com · max · 5-hour session 42%");
  await expect(select.locator("option").nth(1)).toHaveText("two@example.com");
  const codex = page.locator('[data-runner-accounts-picker-row="fmnext-r1:codex"]');
  await expect(codex).toContainText("Not signed in");
  await expect(codex.locator("a")).toHaveText("Connect Codex");
  await expect(codex.locator("a")).toHaveAttribute("href",
    "/dashboard/runners?runner=fmnext-r1&provider=codex&connect=1");

  await select.selectOption(ACCOUNT_B);
  await expect.poll(() => posts.length).toBe(1);
  expect(posts[0]).toEqual({ provider: "claude-code", account: ACCOUNT_B, action: "activate" });
  await expect(page.locator("[data-runner-accounts-picker-notice]")).toContainText("Switching fmnext-r1 to two@example.com");
  await expect(page.locator('[data-runner-accounts-picker-row="fmnext-r1:claude-code"]')).toContainText("Switching...");
  await expect(select).toBeDisabled();
});

test("the runner accounts section stays hidden without runners", async ({ page }) => {
  await stubDashboard(page, AGENTS_PAGE_PATH, "**/dashboard/agents*");
  await page.goto("/dashboard/agents");
  await expect(page.locator("[data-agents-row]")).toHaveCount(3);
  await expect(page.locator("[data-runner-accounts-picker]")).toBeHidden();
});

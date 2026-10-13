const { test, expect } = require("@playwright/test");
const path = require("node:path");

const REPO_PAGE_PATH = path.resolve(
  __dirname, "..", "..", "public", "dashboard", "repo.html",
);

const repositories = (owner) => [{
  owner,
  name: "engine",
  description: "Physics core",
  visibility: "public",
  source: "remote-clone",
  cloneUrl: `https://forkmesh.test/${owner}/engine`,
  rootCommit: "root-0",
  stateHash: "state-0",
  updatedAt: "1",
}];

const FAKE_DESKTOP_MODULE = `
const calls = [];
const owner = window.__desktopRepoOwner || "alpha";
const connects = [];
window.__desktopConnects = connects;
const listeners = {};
let sessions = [
  { id: 11, repositoryOwner: owner, repositoryName: "engine", prompt: "Speed up the solver\\nwith SIMD", status: "running", provider: "claude-code", model: "opus", branchName: "fm/speed-solver", baseBranch: "main", stoppable: true, createdAtMs: 1757000000000, startedAtMs: 1757000001000, numTurns: 3 },
  { id: 12, repositoryOwner: owner.toUpperCase(), repositoryName: "Engine", prompt: "Fix the flaky test", status: "success", provider: "codex", model: "sol", branchName: "fm/fix-flaky", baseBranch: "main", createdAtMs: 1756000000000, finishedAtMs: 1756000500000, costUsd: 0.5 },
  { id: 13, repositoryOwner: owner, repositoryName: "other", prompt: "Not this repository", status: "success", provider: "claude-code", branchName: "fm/other" },
];
let transcript = "==> started\\nreading src/solver.cpp\\n";
window.__desktopCalls = calls;
window.__desktopEmit = (event) => (listeners[event] || []).forEach((fn) => fn({}));
window.__desktopAppend = (text) => { transcript += text; };
const connection = {
  info: { name: "studio", node: "studio" },
  isOpen: true,
  closeReason: "",
  on(event, fn) { (listeners[event] ||= []).push(fn); return () => {}; },
  onClose() { return () => {}; },
  async request(method, params = {}) {
    calls.push({ method, params });
    if (method === "agents.list") {
      return { sessions, repositories: [{ owner, name: "engine" }] };
    }
    if (method === "agents.transcript") {
      const since = params.since ?? -1;
      if (since >= 0) return { id: params.id, transcript: transcript.slice(since), from: since, next: transcript.length };
      return { id: params.id, transcript, from: 0, next: transcript.length };
    }
    if (method === "agentops.diff") {
      return { id: params.id, branch: "fm/fix-flaky", base: "main", files: [{ path: "tests/flaky.cpp", added: 2, removed: 1 }],
        committed: "diff --git a/tests/flaky.cpp b/tests/flaky.cpp\\n@@ -1 +1,2 @@\\n-sleep(1);\\n+wait();\\n+check();\\n",
        uncommitted: "", untracked: ["notes.txt"], truncated: false, hasWorktree: true };
    }
    if (method === "agentops.prepare") {
      return { token: "tok-" + params.action, action: params.action,
        summary: params.action === "merge" ? "Merge fm/fix-flaky into main." : "Delete this agent.",
        typeToConfirm: "" };
    }
    if (method === "agentops.merge") {
      sessions = sessions.map((s) => s.id === params.id ? { ...s, merged: true } : s);
      return { id: params.id, merged: true };
    }
    if (method === "agentops.delete") {
      sessions = sessions.filter((s) => s.id !== params.id);
      return { id: params.id };
    }
    if (method === "agents.stop") {
      sessions = sessions.map((s) => s.id === params.id ? { ...s, status: "stopped", stoppable: false } : s);
      return {};
    }
    return {};
  },
};
export function createDesktopControlHub() {
  return {
    async desktops() { return [{ key: "K1", node: "studio" }, { key: "K2", node: "old-laptop" }]; },
    async connect(node) {
      connects.push(node);
      if (node === "studio") return connection;
      const error = new Error("The desktop did not answer.");
      error.code = "timeout";
      throw error;
    },
    connection(node) { return node === "studio" ? connection : null; },
    closeAll() {},
  };
}
`;

async function openAgentsTab(page, { owner = "alpha", scope = "owner", canViewAgentSessions = true } = {}) {
  await page.addInitScript((repoOwner) => {
    window.__desktopRepoOwner = repoOwner;
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alpha",
      email: "alpha@example.test",
      emailVerified: true,
      sessionToken: "test-session",
      status: "active",
      isAdmin: repoOwner !== "alpha",
    }));
  }, owner);
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.tailwind = {};" }));
  await page.route(
    "https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js",
    (route) => route.fulfill({
      contentType: "text/javascript",
      body: "window.lucide = { createIcons() {} };",
    }));
  await page.route("**/desktop-control.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: FAKE_DESKTOP_MODULE }));
  await page.route(`**/${owner}/engine/agents`, (route) =>
    route.fulfill({ path: REPO_PAGE_PATH, contentType: "text/html; charset=utf-8" }));
  await page.route("**/api/**", async (route) => {
    const url = new URL(route.request().url());
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = { ok: true, repositories: repositories(owner) };
    } else if (/\/agent-(bots|sessions)$/.test(url.pathname)) {
      body = {
        ok: true,
        memberRole: scope === "owner" ? "owner" : "admin",
        scope,
        canQueueAgent: true,
        canViewAgentSessions,
        canDeleteAgentSessions: true,
        sessions: [],
        targets: scope === "owner"
          ? [{ node: "studio", providers: ["claude-code"], online: true, runtimeMode: "desktop" }]
          : [{ node: "teammate-desk", providers: ["claude-code"], online: true, runtimeMode: "desktop" }],
      };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.goto(`/${owner}/engine/agents`);
  await expect(page.locator("[data-desktop-agent-session]")).toHaveCount(2);
}

const SCREENSHOT_DIR = process.env.DESKTOP_AGENTS_SCREENSHOT_DIR || "";

async function shoot(page, name) {
  if (SCREENSHOT_DIR) await page.screenshot({ path: path.join(SCREENSHOT_DIR, `${name}.png`), fullPage: true });
}

async function desktopCalls(page, method) {
  return page.evaluate((name) => window.__desktopCalls.filter((c) => c.method === name), method);
}

test("a connected desktop's agents for this repository join the Agents list", async ({ page }) => {
  await openAgentsTab(page);
  const status = page.locator("[data-desktop-agents-status]");
  await expect(status).toContainText("studio connected, end to end encrypted");
  await expect(status).toContainText("Not reachable: old-laptop");
  const rows = page.locator("[data-desktop-agent-session]");
  await expect(rows.first()).toContainText("Speed up the solver");
  await expect(rows.nth(1)).toContainText("Fix the flaky test");
  await expect(page.locator("[data-desktop-agents]")).not.toContainText("Not this repository");
  await expect(page.locator("[data-org-agent-sessions]")).toContainText("No sessions started from the website yet.");
});

test("an organization repository also lists the viewer's own desktop agents", async ({ page }) => {
  await openAgentsTab(page, { owner: "acme", scope: "organization" });
  await expect(page.locator("[data-desktop-agents]")).toContainText("On desktop");
  await expect(page.locator("[data-desktop-agents-status]")).toContainText("studio connected, end to end encrypted");
  const rows = page.locator("[data-desktop-agent-session]");
  await expect(rows.first()).toContainText("Speed up the solver");
  await expect(rows.nth(1)).toContainText("Fix the flaky test");
  await expect(page.locator("[data-desktop-agents]")).not.toContainText("Not this repository");
  const connects = await page.evaluate(() => window.__desktopConnects);
  expect(connects).not.toContain("teammate-desk");
});

test("desktop agents show even when the member cannot read the website sessions", async ({ page }) => {
  await openAgentsTab(page, { owner: "acme", scope: "organization", canViewAgentSessions: false });
  await page.locator('[data-desktop-agent-open="desktop:studio:11"]').click();
  await expect(page.locator('[data-desktop-agent-detail="desktop:studio:11"]')).toContainText("fm/speed-solver");
});

test("a desktop agent opens with its live transcript, and can be steered and stopped", async ({ page }) => {
  await openAgentsTab(page);
  await page.locator('[data-desktop-agent-open="desktop:studio:11"]').click();
  const detail = page.locator('[data-desktop-agent-detail="desktop:studio:11"]');
  await expect(detail.locator("[data-desktop-agent-transcript]")).toContainText("reading src/solver.cpp");
  await expect(detail).toContainText("fm/speed-solver");
  await shoot(page, "desktop-agent-transcript");
  await expect(detail.locator('[data-desktop-agent-action="merge"]')).toHaveCount(0);

  await page.evaluate(() => {
    window.__desktopAppend("wrote src/simd.cpp\n");
    window.__desktopEmit("agents.changed");
  });
  await expect(detail.locator("[data-desktop-agent-transcript]")).toContainText("wrote src/simd.cpp");
  const reads = await desktopCalls(page, "agents.transcript");
  expect(reads.at(-1).params.since).toBeGreaterThan(0);

  await detail.locator("textarea[name='prompt']").fill("Also add a benchmark");
  await detail.locator("textarea[name='prompt']").press("Enter");
  await expect.poll(async () => (await desktopCalls(page, "agents.prompt")).map((c) => c.params))
    .toEqual([{ id: 11, text: "Also add a benchmark" }]);

  page.once("dialog", (dialog) => dialog.accept());
  await page.locator('[data-desktop-agent-action="stop"]').click();
  await expect.poll(async () => (await desktopCalls(page, "agents.stop")).length).toBe(1);
  await expect(page.locator('[data-desktop-agent-detail="desktop:studio:11"]')).toContainText("stopped");
});

test("a finished desktop agent shows its diff, merges in one click, and deletes", async ({ page }) => {
  await openAgentsTab(page);
  await page.locator('[data-desktop-agent-open="desktop:studio:12"]').click();
  const detail = page.locator('[data-desktop-agent-detail="desktop:studio:12"]');
  await detail.locator('[data-desktop-agent-action="diff"]').click();
  const diff = detail.locator("[data-desktop-agent-diff]");
  await expect(diff).toContainText("tests/flaky.cpp");
  await expect(diff).toContainText("+wait();");
  await expect(diff).toContainText("notes.txt");
  await expect(diff).toContainText("main...fm/fix-flaky");
  await shoot(page, "desktop-agent-diff");

  page.once("dialog", (dialog) => {
    expect(dialog.type()).toBe("confirm");
    expect(dialog.message()).toContain("Merge fm/fix-flaky into main.");
    dialog.accept();
  });
  await detail.locator('[data-desktop-agent-action="merge"]').click();
  await expect.poll(async () => (await desktopCalls(page, "agentops.merge")).map((c) => c.params))
    .toEqual([{ id: 12, token: "tok-merge", typed: "" }]);
  await expect(detail).toContainText("Merged fm/fix-flaky and cleaned up its worktree on the desktop.");
  await expect(detail.locator('[data-desktop-agent-action="merge"]')).toHaveCount(0);

  page.once("dialog", (dialog) => dialog.accept());
  await detail.locator('[data-desktop-agent-action="delete"]').click();
  await expect.poll(async () => (await desktopCalls(page, "agentops.delete")).map((c) => c.params))
    .toEqual([{ id: 12, token: "tok-delete" }]);
  await expect(page.locator("[data-desktop-agent-session]")).toHaveCount(1);
  await expect(page.locator("[data-desktop-agent-detail]")).toHaveCount(0);
});

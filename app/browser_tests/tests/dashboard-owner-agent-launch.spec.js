const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const REPOS_PAGE = path.join(PUBLIC, "dashboard", "repos", "index.html");
const REPOSITORY = {
  owner: "desktop1",
  name: "forkmesh",
  description: "Owner agent launch fixture",
  visibility: "public",
  source: "local-node",
  cloneUrl: "https://forkmesh.test/desktop1/forkmesh",
  logicalOwner: "forkmesh",
  logicalOwners: [{ kind: "organization", owner: "forkmesh" }],
  rootCommit: "abc123",
  stateHash: "state-1",
  updatedAt: "1",
};
const REWRITTEN_MIRROR = {
  ...REPOSITORY,
  owner: "laptop",
  cloneUrl: "https://forkmesh.test/laptop/forkmesh",
  rootCommit: "rewritten-history",
  stateHash: "state-2",
};
const OWNED_TOOLS = {
  ...REPOSITORY,
  owner: "laptop",
  name: "tools",
  cloneUrl: "https://forkmesh.test/laptop/tools",
  logicalOwner: "alice",
  logicalOwners: [{ kind: "user", owner: "alice" }],
  rootCommit: "tools-root",
  stateHash: "tools-state",
};
const UNRELATED_REPOSITORY = {
  ...REPOSITORY,
  owner: "outsider",
  name: "unrelated",
  cloneUrl: "https://forkmesh.test/outsider/unrelated",
  logicalOwner: "outsider",
  logicalOwners: [{ kind: "user", owner: "outsider" }],
  rootCommit: "unrelated-root",
  stateHash: "unrelated-state",
};

test("owner starts a PR-producing agent on a selected owned node", async ({ page }) => {
  let submitted = null;
  const requestedAgentPaths = [];
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alice",
      email: "alice@example.test",
      emailVerified: true,
      sessionToken: "test-session",
      status: "active",
      nodes: ["desktop1", "laptop"],
      isAdmin: true,
    }));
    localStorage.setItem(
      "forkmesh.agent.recentRepositories.v1:alice",
      JSON.stringify(["forkmesh/forkmesh"]),
    );
  });
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.tailwind = {};" }));
  await page.route(
    "https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js",
    (route) => route.fulfill({
      contentType: "text/javascript",
      body: "window.lucide = { createIcons() {} };",
    }),
  );
  await page.route("**/dashboard/repos", (route) => route.fulfill({
    path: REPOS_PAGE,
    contentType: "text/html; charset=utf-8",
  }));
  await page.route("**/dashboard.js*", (route) => route.fulfill({
    path: path.join(PUBLIC, "dashboard.js"),
    contentType: "text/javascript; charset=utf-8",
  }));
  await page.route("**/dashboard/tailwind.css*", (route) => route.fulfill({
    path: path.join(PUBLIC, "dashboard", "tailwind.css"),
    contentType: "text/css; charset=utf-8",
  }));
  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const pathname = new URL(request.url()).pathname;
    if (pathname.includes("agent-sessions")) requestedAgentPaths.push(pathname);
    let body = { ok: true };
    let status = 200;
    if (pathname === "/api/repositories") {
      body = {
        ok: true,
        repositories: [
          REPOSITORY,
          REWRITTEN_MIRROR,
          OWNED_TOOLS,
          UNRELATED_REPOSITORY,
        ],
      };
    } else if (pathname === "/api/orgs") {
      body = { ok: true, orgs: [{ name: "acme", role: "member" }] };
    } else if (pathname === "/api/orgs/acme/repos") {
      body = { ok: true, repos: [{ repo: "shared", node: "acme-node" }] };
    } else if (pathname === "/api/repo/forkmesh/forkmesh/agent-sessions") {
      if (request.method() === "GET") {
        body = {
          ok: true,
          scope: "owner",
          targets: [
            { node: "laptop", providers: ["codex"], online: true, source: false },
            { node: "desktop1", providers: ["claude-code"], online: false, source: true, runtimeMode: "headless" },
            {
              node: "fmnext-n1",
              queueNode: "fmnext-n1",
              runner: "fmnext-r1",
              providers: [],
              needsLogin: { "claude-code": ["fmnext-r1"] },
              online: true,
              source: true,
              runtimeMode: "runner-pool",
            },
          ],
          sessions: [],
        };
      } else {
        submitted = request.postDataJSON();
        status = 202;
        body = {
          ok: true,
          targetNode: submitted.targetNode,
          targetOnline: false,
          queueState: "waiting_for_desktop",
          message: "Saved for desktop1.",
        };
      }
    }
    await route.fulfill({
      status,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });

  await page.goto("/dashboard/repos");
  await page.getByRole("button", { name: "Start an agent on an owned node" }).click();
  const modal = page.locator("#agentModal");
  await expect(modal).toBeVisible();
  await expect(modal.locator("[data-agent-modal-repo]")).toHaveValue(
    "desktop1/forkmesh",
  );
  await expect(modal.locator("[data-agent-modal-repo]")).toContainText(
    "forkmesh/forkmesh",
  );
  await expect(modal.locator("[data-agent-modal-repo] option")).toHaveCount(3);
  await expect(modal.locator("[data-agent-modal-repo] option")).toHaveText([
    "acme/shared",
    "alice/tools",
    "forkmesh/forkmesh",
  ]);
  await expect(modal.locator("[data-agent-modal-repo]")).not.toContainText(
    "outsider/unrelated",
  );
  await expect.poll(() => requestedAgentPaths).toContain(
    "/api/repo/forkmesh/forkmesh/agent-sessions",
  );
  const agentModel = modal.locator("[data-repo-agent-new-agent-model]");
  await expect(agentModel.locator("optgroup")).toHaveCount(3);
  await expect(agentModel).toHaveValue("claude-code|");
  await agentModel.selectOption("claude-code|opus");
  await expect(modal.locator("[data-agent-modal-target]")).toHaveValue("desktop1");
  await expect(modal.locator("[data-agent-modal-target]")).toContainText(
    "offline — queue for later",
  );
  await expect(modal.locator("[data-agent-modal-target]")).toContainText(
    "managed",
  );
  const login = modal.locator("[data-agent-modal-login]");
  await expect(login).toBeVisible();
  await expect(login.locator("[data-agent-modal-login-desktop]")).toHaveAttribute(
    "href",
    "forkmesh://agent-login/desktop1?provider=claude-code",
  );
  await expect(login).toContainText("Sign in to Claude Code on desktop1");
  const target = modal.locator("[data-agent-modal-target]");
  await expect(target.locator('option[value="fmnext-n1"]')).toContainText(
    "fmnext-r1",
  );
  await expect(target.locator('option[value="fmnext-n1"]')).toContainText(
    "needs sign-in",
  );
  await target.selectOption("fmnext-n1");
  await expect(login).toContainText("Sign in to Claude Code on fmnext-r1");
  await expect(login.locator("[data-agent-modal-login-desktop]")).toBeHidden();
  const runnerSignIn = page.context().waitForEvent("page");
  await login.locator("[data-agent-modal-login-start]").click();
  const runnerPage = await runnerSignIn;
  await expect.poll(() => new URL(runnerPage.url()).pathname).toMatch(
    /^\/dashboard\/runners\/?$/,
  );
  expect(Object.fromEntries(new URL(runnerPage.url()).searchParams)).toEqual({
    runner: "fmnext-r1",
    provider: "claude-code",
    connect: "1",
  });
  await runnerPage.close();
  await expect(page.locator("[data-agent-login-modal]")).toBeHidden();
  await target.selectOption("desktop1");
  await expect(login.locator("[data-agent-modal-login-desktop]")).toBeVisible();
  await modal.locator("[data-repo-agent-new-input]").fill("Fix the failing test");
  await modal.getByRole("button", { name: "Start agent" }).click();

  await expect.poll(() => submitted).not.toBeNull();
  expect(submitted).toMatchObject({
    prompt: "Fix the failing test",
    provider: "claude-code",
    model: "opus",
    targetNode: "desktop1",
    createPr: false,
    sessionToken: "test-session",
  });
});

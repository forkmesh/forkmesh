const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const REPO_PAGE_PATH = path.join(PUBLIC, "dashboard", "repo.html");
const DASHBOARD_JS_PATH = path.join(PUBLIC, "dashboard.js");
const DASHBOARD_CSS_PATH = path.join(PUBLIC, "dashboard", "tailwind.css");

const HEAD = "a".repeat(40);

// The website's Actions tab is a window onto a mirror's Actions executor: it
// lists the workflows, shows the executor's runs with their steps and log
// tail, and -- only when that node advertises the actions-control operation --
// sends dispatch / stop / re-run / enable requests back to the same node.
// Contract: www/docs/operations/mirror-actions-control.md.
async function stageRepoPage(page, { session, api }) {
  if (session) {
    await page.addInitScript((value) => {
      localStorage.setItem("forkmesh.session", JSON.stringify(value));
    }, session);
  }
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({
      contentType: "text/javascript",
      body: "window.tailwind = {};",
    }),
  );
  await page.route(
    "https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js",
    (route) => route.fulfill({
      contentType: "text/javascript",
      body: "window.lucide = { createIcons() {} };",
    }),
  );
  await page.route("**/alice/proj/actions", (route) =>
    route.fulfill({
      path: REPO_PAGE_PATH,
      contentType: "text/html; charset=utf-8",
    }),
  );
  await page.route("**/dashboard.js*", (route) =>
    route.fulfill({
      path: DASHBOARD_JS_PATH,
      contentType: "text/javascript; charset=utf-8",
    }),
  );
  await page.route("**/dashboard/tailwind.css", (route) =>
    route.fulfill({
      path: DASHBOARD_CSS_PATH,
      contentType: "text/css; charset=utf-8",
    }),
  );
  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    if (url.pathname === "/api/repositories") {
      return route.fulfill({
        status: 200,
        contentType: "application/json; charset=utf-8",
        body: JSON.stringify({ ok: true, repositories: [REPOSITORY] }),
      });
    }
    const handled = await api(url, request);
    return route.fulfill({
      status: handled?.status || 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(handled?.body ?? { ok: true }),
    });
  });
}

const REPOSITORY = {
  owner: "alice",
  name: "proj",
  visibility: "public",
  source: "local-node",
  cloneUrl: "https://forkmesh.test/alice/proj",
  rootCommit: "root",
  updatedAt: "2",
  liveHost: true,
  cloneOnline: true,
  defaultBranch: "main",
  branchCount: 2,
};

const OWNER = { nodeName: "alice", sessionToken: "test-session", status: "active" };

const CI_WORKFLOW = {
  path: ".forkmesh/ci.yml",
  name: "CI",
  events: ["push", "workflow_dispatch"],
  manual: true,
  enabled: true,
  runsOn: [],
  valid: true,
};

function summary(extra = {}) {
  return {
    ok: true,
    node: "mirror-one",
    updatedAt: Date.now(),
    expiresAt: Date.now() + 600000,
    schemaVersion: 2,
    controlAvailable: true,
    actionsEnabled: true,
    workflows: [
      CI_WORKFLOW,
      { ...CI_WORKFLOW, path: ".forkmesh/deploy.yml", name: "Deploy",
        events: ["release"], manual: false },
    ],
    controls: [],
    runs: [
      {
        id: 12, workflow: "CI", workflowPath: ".forkmesh/ci.yml", commit: HEAD,
        ref: "refs/heads/main", status: "running", pullNumber: 0,
        createdAt: Date.now() - 60000, startedAt: Date.now() - 50000,
        finishedAt: 0,
        steps: [
          { name: "Checkout", status: "success" },
          { name: "Run <tests>", status: "running" },
        ],
        logTail: "==> Run tests\n<img src=x onerror=alert(1)>\n",
      },
      {
        id: 11, workflow: "Deploy", workflowPath: ".forkmesh/deploy.yml",
        commit: HEAD, ref: "refs/heads/main", status: "failed", pullNumber: 0,
        createdAt: Date.now() - 900000, startedAt: Date.now() - 890000,
        finishedAt: Date.now() - 800000, steps: [], logTail: "exit 1",
      },
    ],
    ...extra,
  };
}

function commonApi(url) {
  if (url.pathname === "/api/repo/alice/proj/branches") {
    return {
      body: {
        ok: true, defaultBranch: "main",
        branches: [{ name: "main", commit: HEAD }, { name: "release", commit: HEAD }],
      },
    };
  }
  if (url.pathname === "/api/repo/alice/proj/tree") {
    return { body: { ok: true, entries: [{ type: "blob", name: "ci.yml" }] } };
  }
  if (url.pathname === "/api/repo/alice/proj/blobs") {
    return {
      body: {
        ok: true,
        blobs: {
          ".forkmesh/ci.yml": {
            content: "name: CI\non:\n  push:\n  workflow_dispatch:\nsteps:\n  - run: make test\n",
          },
        },
      },
    };
  }
  return null;
}

test("an owner sees workflows, runs, steps and the log, and can dispatch a run", async ({
  page,
}) => {
  const controls = [];
  let outcome = null;
  await stageRepoPage(page, {
    session: OWNER,
    api: async (url, request) => {
      if (url.pathname === "/api/repo/alice/proj/actions/runs") {
        return { body: summary(outcome ? { controls: [outcome] } : {}) };
      }
      if (url.pathname === "/api/repo/alice/proj/actions/control") {
        const body = request.postDataJSON();
        controls.push(body);
        outcome = {
          requestId: body.requestId, action: body.action, status: "applied",
          reason: "", runId: 13, finishedAt: Date.now(),
        };
        return {
          status: 202,
          body: { ok: true, status: "accepted", requestId: body.requestId, node: "mirror-one" },
        };
      }
      return commonApi(url);
    },
  });

  await page.goto("/alice/proj/actions");
  const tab = page.locator('[data-dashboard-repo-tab="actions"]');
  await expect(tab).toHaveAttribute("aria-selected", "true");
  const panel = page.locator("[data-repo-actions]");
  await expect(panel).toContainText("Runs on mirror");
  await expect(panel).toContainText("mirror-one");

  // Workflows come from the executor's summary, with their triggers.
  const workflows = panel.locator("[data-repo-actions-workflows]");
  await expect(workflows).toContainText("All workflows");
  await expect(workflows).toContainText("on: push, workflow_dispatch");
  await expect(workflows).toContainText("Deploy");

  // Both runs are listed; selecting a workflow filters them.
  await expect(panel.locator("[data-repo-actions-run]")).toHaveCount(2);
  await workflows.locator('[data-repo-actions-workflow=".forkmesh/ci.yml"]').click();
  await expect(panel.locator("[data-repo-actions-run]")).toHaveCount(1);

  // A run opens to its steps and log tail; executor text is never markup.
  await panel.locator('[data-repo-actions-run-toggle="12"]').click();
  const detail = panel.locator('[data-repo-actions-run-detail="12"]');
  await expect(detail).toContainText("Checkout");
  await expect(detail).toContainText("Run <tests>");
  await expect(detail.locator("[data-repo-actions-log]")).toContainText(
    "<img src=x onerror=alert(1)>");
  await expect(detail.locator("img")).toHaveCount(0);
  await expect(detail.locator('[data-repo-actions-run-control="cancel"]')).toBeVisible();

  // Manual dispatch names the workflow, the branch ref and the executor node.
  await panel.locator("[data-repo-actions-dispatch-ref]").selectOption("release");
  await panel.locator('[data-repo-actions-dispatch] button[type="submit"]').click();
  await expect.poll(() => controls.length).toBe(1);
  expect(controls[0]).toMatchObject({
    action: "dispatch",
    node: "mirror-one",
    workflow: ".forkmesh/ci.yml",
    ref: "refs/heads/release",
  });
  expect(controls[0].requestId).toMatch(/^[A-Za-z0-9_-]{16,64}$/);
  await expect(panel).toContainText("waiting for the executor");
  // The executor's answer arrives with a later summary.
  await expect(panel).toContainText("Run ci.yml on release: done.", { timeout: 12000 });
});

test("a read-only executor shows runs but offers no controls", async ({ page }) => {
  let posted = 0;
  await stageRepoPage(page, {
    session: OWNER,
    api: async (url) => {
      if (url.pathname === "/api/repo/alice/proj/actions/runs") {
        // Summary v1 from a node that does not advertise actions-control.
        return {
          body: {
            ok: true, node: "mirror-one", updatedAt: Date.now(),
            expiresAt: Date.now() + 600000, controlAvailable: false,
            runs: [{
              id: 3, workflow: "CI", commit: HEAD, ref: "refs/heads/main",
              status: "success", createdAt: 1, startedAt: 2, finishedAt: 3,
              logTail: "ok",
            }],
          },
        };
      }
      if (url.pathname === "/api/repo/alice/proj/actions/control") posted += 1;
      return commonApi(url);
    },
  });
  await page.goto("/alice/proj/actions");
  const panel = page.locator("[data-repo-actions]");
  await expect(panel).toContainText("publishes runs read-only");
  // Without a v2 summary the workflow list is read from .forkmesh/ itself.
  await expect(panel.locator("[data-repo-actions-workflows]")).toContainText(
    "on: push, workflow_dispatch");
  await panel.locator('[data-repo-actions-run-toggle="3"]').click();
  await expect(panel.locator('[data-repo-actions-run-detail="3"]')).toContainText("ok");
  await expect(panel.locator("[data-repo-actions-run-control]")).toHaveCount(0);
  await expect(panel.locator("[data-repo-actions-dispatch]")).toHaveCount(0);
  expect(posted).toBe(0);
});

test("no executor and no session each get an explanation, not an error", async ({
  page,
}) => {
  await stageRepoPage(page, {
    session: OWNER,
    api: async (url) => {
      if (url.pathname === "/api/repo/alice/proj/actions/runs") {
        return { body: { ok: false, state: "unavailable", runs: [], retryable: true } };
      }
      return commonApi(url);
    },
  });
  await page.goto("/alice/proj/actions");
  const panel = page.locator("[data-repo-actions]");
  await expect(panel).toContainText("No runs reported for this repository yet");
  await expect(panel).toContainText("ForkMesh runner");
  // The workflow list needs no executor.
  await expect(panel.locator("[data-repo-actions-workflows]")).toContainText("CI");
});

test("a signed-out visitor sees the workflows but is never asked for runs", async ({
  page,
}) => {
  let runReads = 0;
  await stageRepoPage(page, {
    session: null,
    api: async (url) => {
      if (url.pathname === "/api/repo/alice/proj/actions/runs") runReads += 1;
      return commonApi(url);
    },
  });
  await page.goto("/alice/proj/actions");
  const panel = page.locator("[data-repo-actions]");
  await expect(panel).toContainText("Sign in to see runs");
  await expect(panel.locator("[data-repo-actions-workflows]")).toContainText("CI");
  expect(runReads).toBe(0);
});

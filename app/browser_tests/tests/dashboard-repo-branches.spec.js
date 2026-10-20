const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const REPO_PAGE_PATH = path.join(PUBLIC, "dashboard", "repo.html");
const DASHBOARD_JS_PATH = path.join(PUBLIC, "dashboard.js");
const DASHBOARD_CSS_PATH = path.join(PUBLIC, "dashboard", "tailwind.css");

const OLD = "a".repeat(40);
const TIP = "b".repeat(40);
const STALE = "c".repeat(40);
const DIVERGED = "d".repeat(40);

// The website's Branches page is the desktop app's branch table: the same
// behind/ahead divergence, the same conflict verdict, and the same delete /
// update / create / delete-merged actions — queued for the node that owns the
// repository's checkout, because the relay has no Git.
async function stageRepoPage(page, { session, repositories, api }) {
  await page.addInitScript((value) => {
    localStorage.setItem("forkmesh.session", JSON.stringify(value));
  }, session);
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
  await page.route("**/alice/proj/branches", (route) =>
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
        body: JSON.stringify({ ok: true, repositories }),
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
  branchCount: 4,
};

const BRANCH_LISTING = {
  ok: true,
  defaultBranch: "main",
  branches: [
    {
      name: "main",
      commit: TIP,
      updatedAt: "2026-09-10T12:00:00-04:00",
      message: "Ship the branches page",
      author: "Alice",
      ahead: 0,
      behind: 0,
    },
    {
      name: "agent/merged-work",
      commit: OLD,
      updatedAt: "2026-09-09T12:00:00-04:00",
      message: "Already landed",
      author: "Agent",
      ahead: 0,
      behind: 3,
    },
    {
      name: "feature/stale",
      commit: STALE,
      updatedAt: "2026-09-08T12:00:00-04:00",
      message: "Waiting on review",
      author: "Bob",
      ahead: 2,
      behind: 5,
    },
    {
      name: "feature/clean",
      commit: DIVERGED,
      updatedAt: "2026-09-07T12:00:00-04:00",
      message: "Small change",
      author: "Carol",
      ahead: 1,
      behind: 1,
    },
  ],
};

test("the branches page shows divergence and conflicts from the mirror", async ({
  page,
}) => {
  const previews = [];
  await stageRepoPage(page, {
    session: { nodeName: "alice", sessionToken: "test-session", status: "active" },
    repositories: [REPOSITORY],
    api: async (url) => {
      if (url.pathname === "/api/repo/alice/proj/branches") {
        return { body: BRANCH_LISTING };
      }
      if (url.pathname === "/api/repo/alice/proj/merge-preview") {
        const head = url.searchParams.get("head");
        previews.push({ base: url.searchParams.get("base"), head });
        return {
          body: head === "feature/stale"
            ? { ok: true, ahead: 2, behind: 5, clean: false,
                conflicts: ["src/app.py"] }
            : { ok: true, ahead: 1, behind: 1, clean: true, conflicts: [] },
        };
      }
      if (url.pathname === "/api/repo/alice/proj/branches/ops") {
        return { body: { ok: true, ops: [] } };
      }
      return { body: { ok: true } };
    },
  });

  await page.goto("/alice/proj/branches");

  const rows = page.locator("[data-repo-branches-row]");
  await expect(rows).toHaveCount(4);
  await expect(page.locator("[data-repo-branches-total]")).toHaveText("4 branches");

  const row = (name) =>
    page.locator(`[data-repo-branches-row-name="${name}"]`);
  await expect(row("main").locator("[data-repo-branch-divergence]"))
    .toHaveText("default branch");
  await expect(row("agent/merged-work").locator("[data-repo-branch-divergence]"))
    .toHaveText("merged into main");
  await expect(row("feature/stale").locator("[data-repo-branch-divergence]"))
    .toHaveText("5 behind · 2 ahead");

  // Only branches that diverged BOTH ways can conflict, so only those are
  // tried against the default branch on the mirror.
  await expect(row("feature/stale").locator("[data-repo-branch-conflicts]"))
    .toBeVisible();
  await expect(row("feature/clean").locator("[data-repo-branch-conflicts]"))
    .toHaveCount(0);
  expect(previews).toEqual([
    { base: "main", head: "feature/stale" },
    { base: "main", head: "feature/clean" },
  ]);
});

function branchDeleteApi({ deletes, answer, listing = () => BRANCH_LISTING }) {
  let pending = [];
  const queued = [];
  const handler = async (url, request) => {
    if (url.pathname === "/api/repo/alice/proj/branches") {
      return { body: listing() };
    }
    if (url.pathname === "/api/repo/alice/proj/merge-preview") {
      return { body: { ok: true, clean: true, conflicts: [] } };
    }
    if (url.pathname === "/api/repo/alice/proj/branches/delete") {
      const body = request.postDataJSON();
      deletes.push({ authorization: request.headers().authorization, body });
      const reply = answer(body);
      pending = pending.concat(reply.results
        .filter((result) => result.status === "queued")
        .map((result) => ({
          id: pending.length + 1, op: "delete", branch: result.branch,
          base: "", expectedOid: OLD, startOid: "", requestedBy: "alice", queuedAt: 1,
        })));
      return { body: { ok: true, ...reply, ops: pending } };
    }
    if (url.pathname === "/api/repo/alice/proj/branches/ops") {
      if (request.method() === "POST") {
        const body = request.postDataJSON();
        queued.push({ authorization: request.headers().authorization, body });
        pending = pending.concat([{
          id: pending.length + 1,
          op: body.op,
          branch: body.branch,
          base: body.base || "",
          expectedOid: body.expectedOid || "",
          startOid: body.startOid || "",
          requestedBy: "alice",
          queuedAt: 1,
        }]);
      }
      if (request.method() === "DELETE") pending = [];
      return { body: { ok: true, queued: true, ops: pending } };
    }
    return { body: { ok: true } };
  };
  return { handler, queued };
}

function failOnDialogs(page) {
  page.on("dialog", (dialog) => {
    throw new Error(`unexpected ${dialog.type()} dialog: ${dialog.message()}`);
  });
}

test("the owner deletes, updates and creates branches from the web", async ({
  page,
}) => {
  const deletes = [];
  const api = branchDeleteApi({
    deletes,
    answer: (body) => ({
      results: body.branches.map((item) => ({
        branch: item.name, status: "queued", error: "",
        message: `Queued the deletion of ${item.name} for this repository's node.`,
      })),
    }),
  });
  const { queued } = api;
  await stageRepoPage(page, {
    session: { nodeName: "alice", sessionToken: "test-session", status: "active" },
    repositories: [REPOSITORY],
    api: api.handler,
  });

  await page.goto("/alice/proj/branches");
  const row = (name) => page.locator(`[data-repo-branches-row-name="${name}"]`);
  await expect(row("feature/stale")).toBeVisible();

  // The default branch is never offered for deletion or selection.
  await expect(row("main").locator("[data-repo-branch-delete]")).toHaveCount(0);
  await expect(row("main").locator("[data-repo-branch-select]")).toHaveCount(0);

  // Deleting asks inline in the row, and Cancel sends nothing.
  failOnDialogs(page);
  await row("agent/merged-work").locator("[data-repo-branch-delete]").click();
  const confirmation = row("agent/merged-work").locator("[data-repo-branch-delete-confirmation]");
  await expect(confirmation).toContainText("Delete agent/merged-work?");
  await confirmation.locator("[data-repo-branch-delete-cancel]").click();
  await expect(confirmation).toHaveCount(0);
  expect(deletes).toEqual([]);

  // Confirming sends the exact tip the page showed.
  await row("agent/merged-work").locator("[data-repo-branch-delete]").click();
  await row("agent/merged-work").locator("[data-repo-branch-delete-confirm]").click();
  await expect(page.locator("[data-repo-branches-status]")).toContainText(
    "Queued the deletion of agent/merged-work",
  );
  await expect(row("agent/merged-work").locator("[data-repo-branch-pending]"))
    .toContainText("deletion queued");
  // A queued branch has no second delete offered until the node takes it.
  await expect(row("agent/merged-work").locator("[data-repo-branch-delete]"))
    .toBeDisabled();
  expect(deletes).toEqual([{
    authorization: "Bearer test-session",
    body: { branches: [{ name: "agent/merged-work", expectedOid: OLD }] },
  }]);
  page.removeAllListeners("dialog");

  // "Update from main" is only offered for a branch that is actually behind.
  await expect(row("main").locator("[data-repo-branch-update]")).toHaveCount(0);
  await row("feature/stale").locator("[data-repo-branch-update]").click();
  await expect(page.locator("[data-repo-branches-status]")).toContainText(
    "Queued an update of feature/stale from main",
  );
  expect(queued[0].body).toEqual({
    op: "update",
    branch: "feature/stale",
    expectedOid: STALE,
    base: "main",
    defaultBranch: "main",
  });

  // A new branch starts at the default branch's current tip.
  page.once("dialog", (dialog) => dialog.accept("release/2026-09"));
  await page.locator("[data-repo-branch-create]").click();
  await expect(page.locator("[data-repo-branches-status]")).toContainText(
    "Queued the creation of release/2026-09 from main",
  );
  expect(queued[1].body).toEqual({
    op: "create",
    branch: "release/2026-09",
    startOid: TIP,
    defaultBranch: "main",
  });

  // Cancelling a queued operation takes it back off the node's queue.
  await row("agent/merged-work")
    .locator("[data-repo-branch-op-cancel]").click();
  await expect(page.locator("[data-repo-branches-status]")).toContainText(
    "Cancelled the queued branch operation",
  );
  await expect(row("agent/merged-work").locator("[data-repo-branch-pending]"))
    .toHaveCount(0);
});

test("the merged filter preselects merged branches for one deletion", async ({ page }) => {
  const deletes = [];
  let deleted = new Set();
  const api = branchDeleteApi({
    deletes,
    listing: () => ({
      ...BRANCH_LISTING,
      branches: BRANCH_LISTING.branches.filter((branch) => !deleted.has(branch.name)),
    }),
    answer: (body) => {
      body.branches.forEach((item) => deleted.add(item.name));
      return {
        results: body.branches.map((item) => ({
          branch: item.name, status: "deleted", error: "", message: `Deleted ${item.name}.`,
        })),
      };
    },
  });
  await stageRepoPage(page, {
    session: { nodeName: "alice", sessionToken: "test-session", status: "active" },
    repositories: [REPOSITORY],
    api: api.handler,
  });
  failOnDialogs(page);

  await page.goto("/alice/proj/branches");
  const filter = page.locator("[data-repo-branches-filter-merged]");
  // Only agent/merged-work contributes nothing of its own.
  await expect(filter).toContainText("Merged into main");
  await expect(filter).toContainText("1");
  await expect(filter).toHaveAttribute("aria-pressed", "false");
  await filter.click();
  await expect(filter).toHaveAttribute("aria-pressed", "true");
  await expect(page.locator("[data-repo-branches-row]")).toHaveCount(1);
  await expect(page.locator('[data-repo-branch-select="agent/merged-work"]')).toBeChecked();
  await expect(page.locator("[data-repo-branches-selected-count]")).toHaveText("1 selected");

  await page.locator("[data-repo-branches-delete-selected]").click();
  const summary = page.locator("[data-repo-branches-delete-summary]");
  await expect(summary).toContainText("Delete 1 branch?");
  await expect(summary).toContainText("All of them are merged into main.");
  await expect(summary.locator("[data-repo-branches-delete-names]")).toHaveText("agent/merged-work");
  expect(deletes).toEqual([]);
  await summary.locator("[data-repo-branches-delete-selected-confirm]").click();

  await expect.poll(() => deletes.map((entry) => entry.body)).toEqual([{
    branches: [{ name: "agent/merged-work", expectedOid: OLD }],
  }]);
  await expect(page.locator("[data-repo-branches-status]")).toContainText("Deleted agent/merged-work.");
  await expect(page.locator('[data-repo-branch-delete-result-row][data-repo-branch-delete-result-status="deleted"]'))
    .toHaveCount(1);
  await expect(page.locator('[data-repo-branches-row-name="agent/merged-work"]')).toHaveCount(0);
  await expect(page.locator("[data-repo-branches-selection]")).toBeHidden();

  // Turning the filter off shows every branch again.
  await page.locator("[data-repo-branches-filter-merged]").click();
  await expect(page.locator("[data-repo-branches-row]")).toHaveCount(3);
});

test("selected branches are deleted together and each reports its own result", async ({
  page,
}) => {
  const deletes = [];
  let deleted = new Set();
  const api = branchDeleteApi({
    deletes,
    listing: () => ({
      ...BRANCH_LISTING,
      branches: BRANCH_LISTING.branches.filter((branch) => !deleted.has(branch.name)),
    }),
    answer: (body) => ({
      results: body.branches.map((item) => {
        if (item.name === "feature/stale") {
          return { branch: item.name, status: "refused", error: "stale_head",
                   message: "feature/stale moved since the page read it, so it was kept. Refresh and try again." };
        }
        deleted.add(item.name);
        return { branch: item.name, status: "deleted", error: "", message: `Deleted ${item.name}.` };
      }),
    }),
  });
  await stageRepoPage(page, {
    session: { nodeName: "alice", sessionToken: "test-session", status: "active" },
    repositories: [REPOSITORY],
    api: api.handler,
  });
  failOnDialogs(page);

  await page.goto("/alice/proj/branches");
  await page.locator('[data-repo-branch-select="feature/stale"]').check();
  await page.locator('[data-repo-branch-select="feature/clean"]').check();
  await expect(page.locator("[data-repo-branches-selected-count]")).toHaveText("2 selected");
  await page.locator("[data-repo-branches-delete-selected]").click();
  const summary = page.locator("[data-repo-branches-delete-summary]");
  await expect(summary).toContainText("Delete 2 branches?");
  await expect(summary).toContainText("2 of them have commits not merged into main.");
  // Cancel keeps the selection and sends nothing.
  await summary.locator("[data-repo-branches-delete-selected-cancel]").click();
  await expect(page.locator("[data-repo-branches-selected-count]")).toHaveText("2 selected");
  expect(deletes).toEqual([]);

  await page.locator("[data-repo-branches-delete-selected]").click();
  await page.locator("[data-repo-branches-delete-selected-confirm]").click();
  await expect.poll(() => deletes.map((entry) => entry.body)).toEqual([{
    branches: [
      { name: "feature/stale", expectedOid: STALE },
      { name: "feature/clean", expectedOid: DIVERGED },
    ],
  }]);
  await expect(page.locator("[data-repo-branches-status]"))
    .toContainText("Deleting 2 branches: 1 deleted, 1 not deleted.");
  const results = page.locator("[data-repo-branch-delete-result-row]");
  await expect(results).toHaveCount(2);
  await expect(page.locator('[data-repo-branch-delete-result-branch="feature/stale"]'))
    .toContainText("moved since the page read it");
  await expect(page.locator('[data-repo-branch-delete-result-branch="feature/clean"]'))
    .toContainText("Deleted feature/clean.");
  // The kept branch stays listed, marked, and still selected for a retry.
  const stale = page.locator('[data-repo-branches-row-name="feature/stale"]');
  await expect(stale.locator('[data-repo-branch-delete-result="refused"]')).toHaveText("not deleted");
  await expect(stale.locator("[data-repo-branch-select]")).toBeChecked();
  await expect(page.locator('[data-repo-branches-row-name="feature/clean"]')).toHaveCount(0);

  await page.locator("[data-repo-branch-delete-results-clear]").click();
  await expect(results).toHaveCount(0);
});

test("every branch behind the default one is updatable in one action", async ({
  page,
}) => {
  const updates = [];
  await stageRepoPage(page, {
    session: { nodeName: "alice", sessionToken: "test-session", status: "active" },
    repositories: [REPOSITORY],
    api: async (url, request) => {
      if (url.pathname === "/api/repo/alice/proj/branches") {
        return { body: BRANCH_LISTING };
      }
      if (url.pathname === "/api/repo/alice/proj/merge-preview") {
        return { body: { ok: true, clean: true, conflicts: [] } };
      }
      if (url.pathname === "/api/repo/alice/proj/branches/ops") {
        if (request.method() === "POST") {
          updates.push(request.postDataJSON());
        }
        return { body: { ok: true, queued: true, queuedCount: 3, ops: [] } };
      }
      return { body: { ok: true } };
    },
  });

  await page.goto("/alice/proj/branches");
  const updateAll = page.locator("[data-repo-branch-update-all]");
  // Three of the four branches are behind main; main itself is not.
  await expect(updateAll).toContainText("Update 3 from main");
  page.once("dialog", (dialog) => dialog.accept());
  await updateAll.click();
  await expect.poll(() => updates.length).toBe(1);
  const ops = updates[0].ops;
  expect(ops.map((update) => update.branch)).toEqual([
    "agent/merged-work", "feature/stale", "feature/clean",
  ]);
  expect(ops.every((update) => update.op === "update"
    && update.base === "main")).toBe(true);
});


test("a visitor who cannot manage the repo sees a read-only list", async ({
  page,
}) => {
  const writes = [];
  await stageRepoPage(page, {
    session: { nodeName: "dana", sessionToken: "test-session", status: "active" },
    repositories: [REPOSITORY],
    api: async (url, request) => {
      if (url.pathname === "/api/repo/alice/proj/branches") {
        return { body: BRANCH_LISTING };
      }
      if (url.pathname === "/api/repo/alice/proj/merge-preview") {
        return { body: { ok: true, clean: true, conflicts: [] } };
      }
      if (url.pathname === "/api/orgs/alice") {
        // Not an organization at all, so no role can be inherited from one.
        return { status: 404, body: { error: "not_found" } };
      }
      if (url.pathname === "/api/repo/alice/proj/branches/ops") {
        writes.push(request.method());
        return { body: { ok: true, ops: [] } };
      }
      return { body: { ok: true } };
    },
  });

  await page.goto("/alice/proj/branches");
  await expect(page.locator("[data-repo-branches-row]")).toHaveCount(4);
  // The divergence a reader can act on stays; the write controls do not.
  await expect(
    page.locator('[data-repo-branches-row-name="feature/stale"] [data-repo-branch-divergence]'),
  ).toHaveText("5 behind · 2 ahead");
  await expect(page.locator("[data-repo-branch-delete]")).toHaveCount(0);
  await expect(page.locator("[data-repo-branch-update]")).toHaveCount(0);
  await expect(page.locator("[data-repo-branch-create]")).toHaveCount(0);
  await expect(page.locator("[data-repo-branches-filter-merged]")).toHaveCount(0);
  await expect(page.locator("[data-repo-branch-select]")).toHaveCount(0);
  await expect(page.locator("[data-repo-branch-update-all]")).toHaveCount(0);
  expect(writes).toEqual([]);
});

function mergeApi({ queued, results }) {
  let pending = [];
  let polls = 0;
  return async (url, request) => {
    if (url.pathname === "/api/repo/alice/proj/branches") {
      return { body: BRANCH_LISTING };
    }
    if (url.pathname === "/api/repo/alice/proj/merge-preview") {
      const head = url.searchParams.get("head");
      const base = url.searchParams.get("base");
      if (head === "feature/stale") {
        return { body: { ok: true, ahead: 2, behind: 5, clean: false,
                         conflicts: ["src/app.py"], baseOid: TIP, headOid: STALE } };
      }
      return { body: { ok: true, ahead: 1, behind: 1, clean: true, conflicts: [],
                       baseOid: base === "main" ? TIP : DIVERGED,
                       headOid: head === "main" ? TIP : DIVERGED } };
    }
    if (url.pathname === "/api/repo/alice/proj/compare") {
      return { body: { ok: true, baseOid: TIP, headOid: DIVERGED, mergeBaseOid: OLD,
                       commitCount: 1, patch: "", commits: "" } };
    }
    if (url.pathname === "/api/repo/alice/proj/branches/ops") {
      if (request.method() === "POST") {
        const body = request.postDataJSON();
        queued.push(body);
        pending = [{ id: 9, op: body.op, branch: body.branch, base: body.base || "",
                     expectedOid: body.expectedOid, baseOid: body.baseOid || "",
                     deleteBranch: body.deleteBranch === true,
                     requestedBy: "alice", queuedAt: 1 }];
        return { body: { ok: true, queued: true, ops: pending } };
      }
      if (pending.length) {
        polls += 1;
        if (polls >= 1) {
          const op = pending[0];
          pending = [];
          results.unshift({
            id: 100 + results.length, requestId: op.id, op: op.op,
            branch: op.branch, base: op.base, status: "applied",
            message: `merged ${op.branch} into ${op.base}; deleted ${op.branch}`,
            mergeOid: "e".repeat(40), branchDeleted: true, reportedAt: Date.now(),
          });
        }
      }
      return { body: { ok: true, ops: pending, results } };
    }
    if (url.pathname === "/api/repo/alice/proj/branches/ops/results") {
      results.length = 0;
      return { body: { ok: true, results: [] } };
    }
    return { body: { ok: true } };
  };
}

test("the owner merges a branch into main and sees what the node did", async ({
  page,
}) => {
  const queued = [];
  const results = [];
  await stageRepoPage(page, {
    session: { nodeName: "alice", sessionToken: "test-session", status: "active" },
    repositories: [REPOSITORY],
    api: mergeApi({ queued, results }),
  });

  await page.goto("/alice/proj/branches");
  const row = (name) => page.locator(`[data-repo-branches-row-name="${name}"]`);
  await expect(row("feature/clean")).toBeVisible();
  await expect(row("main").locator("[data-repo-branch-merge]")).toHaveCount(0);
  await expect(row("agent/merged-work").locator("[data-repo-branch-merge]"))
    .toHaveCount(0);

  await row("feature/stale").locator("[data-repo-branch-merge]").click();
  const dialog = page.locator("[data-repo-branch-merge-dialog]");
  await expect(dialog).toBeVisible();
  await expect(dialog.locator("[data-repo-branch-merge-preview]"))
    .toContainText("src/app.py");
  await expect(dialog.locator("[data-repo-branch-merge-confirm]")).toBeDisabled();
  await dialog.locator("[data-repo-branch-merge-cancel]").first().click();
  await expect(dialog).toBeHidden();
  expect(queued).toEqual([]);

  await row("feature/clean").locator("[data-repo-branch-merge]").click();
  await expect(dialog.locator("[data-repo-branch-merge-preview]"))
    .toContainText("No conflicts");
  await expect(dialog).toContainText("Merge feature/clean into main");
  await expect(dialog.locator("[data-repo-branch-merge-delete]")).toBeChecked();
  await dialog.locator("[data-repo-branch-merge-confirm]").click();

  await expect.poll(() => queued.length).toBe(1);
  expect(queued[0]).toEqual({
    op: "merge",
    branch: "feature/clean",
    expectedOid: DIVERGED,
    base: "main",
    baseOid: TIP,
    deleteBranch: true,
    defaultBranch: "main",
  });
  await expect(row("feature/clean").locator("[data-repo-branch-pending]"))
    .toContainText("merge queued");

  const result = page.locator("[data-repo-branch-results] [data-repo-branch-result]");
  await expect(result).toContainText("merged feature/clean into main", { timeout: 15000 });
  await expect(result).toHaveAttribute("data-repo-branch-result-status", "applied");

  await page.locator("[data-repo-branch-results-clear]").click();
  await expect(page.locator("[data-repo-branch-results] [data-repo-branch-result]"))
    .toHaveCount(0);
});

test("the compare page merges the compared branch into its base", async ({
  page,
}) => {
  const queued = [];
  await stageRepoPage(page, {
    session: { nodeName: "alice", sessionToken: "test-session", status: "active" },
    repositories: [REPOSITORY],
    api: mergeApi({ queued, results: [] }),
  });
  await page.route("**/alice/proj/compare/**", (route) =>
    route.fulfill({ path: REPO_PAGE_PATH, contentType: "text/html; charset=utf-8" }));

  await page.goto("/alice/proj/compare/main...feature/clean");
  const panel = page.locator("[data-repo-compare-merge]");
  await expect(panel.locator("[data-repo-compare-merge-preview]"))
    .toContainText("No conflicts");
  await expect(panel.locator("[data-repo-compare-update]"))
    .toContainText("Update feature/clean from main");

  await panel.locator("[data-repo-compare-merge-open]").click();
  const dialog = page.locator("[data-repo-branch-merge-dialog]");
  await dialog.locator("[data-repo-branch-merge-delete]").uncheck();
  await dialog.locator("[data-repo-branch-merge-confirm]").click();
  await expect.poll(() => queued.length).toBe(1);
  expect(queued[0]).toMatchObject({
    op: "merge",
    branch: "feature/clean",
    base: "main",
    expectedOid: DIVERGED,
    baseOid: TIP,
    deleteBranch: false,
  });
  await expect(panel.locator("[data-repo-compare-merge-status]"))
    .toContainText("Queued the merge of feature/clean into main");
});

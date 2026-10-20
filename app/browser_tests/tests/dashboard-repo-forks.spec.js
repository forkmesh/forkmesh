const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const REPO_PAGE_PATH = path.join(PUBLIC, "dashboard", "repo.html");
const DASHBOARD_JS_PATH = path.join(PUBLIC, "dashboard.js");
const DASHBOARD_CSS_PATH = path.join(PUBLIC, "dashboard", "tailwind.css");

// Clicking the fork icon on a repo shows who forked it. The button carries the
// real count from GET /api/repo/o/r/forks and opens a popover listing each fork
// as <owner>/<name>, linking to that fork's own repository page.
test("the fork icon opens the list of people who forked the repo", async ({
  page,
}) => {
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "dana",
      sessionToken: "test-session",
      status: "active",
    }));
  });
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({
      contentType: "text/javascript",
      body: "window.tailwind = {};",
    }),
  );
  await page.route(
    "https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js",
    (route) =>
      route.fulfill({
        contentType: "text/javascript",
        body: "window.lucide = { createIcons() {} };",
      }),
  );
  await page.route("**/alice/proj", (route) =>
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

  let forkRequests = 0;
  let forkCreate = null;
  await page.route("**/api/**", (route) => {
    const request = route.request();
    const url = new URL(route.request().url());
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = {
        ok: true,
        repositories: [
          {
            owner: "alice",
            name: "proj",
            visibility: "public",
            source: "local-node",
            cloneUrl: "https://forkmesh.test/alice/proj",
            rootCommit: "root",
            updatedAt: "2",
          },
        ],
      };
    } else if (url.pathname === "/api/repo/alice/proj/forks") {
      if (request.method() === "POST") {
        forkCreate = {
          authorization: request.headers().authorization,
          body: request.postDataJSON(),
        };
        body = {
          ok: true,
          queued: true,
          created: false,
          fork: {
            requestId: "0123456789abcdef0123456789abcdef0123",
            owner: "dana-mirror",
            name: "proj",
            status: "pending",
            url: "/dana-mirror/proj",
          },
        };
      } else {
        forkRequests += 1;
        body = {
          ok: true,
          count: 2,
          forks: [
            { owner: "carol", name: "proj", url: "/carol/proj",
              profileUrl: "/@carol", createdAt: 20 },
            { owner: "bob", name: "proj", url: "/bob/proj", createdAt: 10 },
          ],
        };
      }
    }
    return route.fulfill({
      status: request.method() === "POST" ? 202 : 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });

  await page.goto("/alice/proj");

  const forkButton = page.locator("[data-repo-action-fork]");
  await expect(forkButton).toBeVisible();
  // Real count, not a placeholder and not a mock number.
  await expect(page.locator("[data-repo-about-actions] [data-repo-fork-count]")).toHaveText("2");

  const menu = page.locator("[data-repo-fork-menu]");
  await expect(menu).toBeHidden();
  await forkButton.click();
  await expect(menu).toBeVisible();
  await expect(forkButton).toHaveAttribute("aria-expanded", "true");

  // Forking stays entirely on the website: it records a durable job for the
  // caller's managed mirror instead of launching a desktop protocol URL.
  const create = menu.locator("[data-repo-fork-create]");
  await expect(create).toHaveAttribute("type", "button");
  await expect(menu).toContainText("has to be online right now");
  await create.click();
  await expect(create).toContainText("Fork queued");
  await expect(page.locator("[data-repo-fork-status]")).toContainText(
    "Queued on dana-mirror",
  );
  await expect(page.locator('[data-repo-fork-status] a[href="/dana-mirror/proj"]')).toBeVisible();
  expect(forkCreate).toEqual({
    authorization: "Bearer test-session",
    body: { sessionToken: "test-session" },
  });

  const list = page.locator("[data-repo-fork-list]");
  await expect(list).toContainText("carol");
  await expect(list).toContainText("bob");
  await expect(list.locator('a[href="/carol/proj"]')).toBeVisible();
  await expect(list.locator('a[href="/@carol"]')).toBeVisible();
  // bob has no live account row, so the row carries no profile link.
  await expect(list.locator('a[href="/@bob"]')).toHaveCount(0);
  // Opening refetches, so a fork published while the page sat there shows up.
  expect(forkRequests).toBeGreaterThan(1);

  // A click outside closes it again.
  await page.locator("[data-repo-about-actions]").click({ position: { x: 2, y: 2 } });
  await expect(menu).toBeHidden();
  await expect(forkButton).toHaveAttribute("aria-expanded", "false");
});

test("a fork submits its portable pull request to an offline parent", async ({
  page,
}) => {
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "bob",
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
  await page.route(/https?:\/\/[^/]+\/(?:bob\/proj\/pulls|alice\/proj\/pulls\/41)$/, (route) =>
    route.fulfill({
      path: REPO_PAGE_PATH,
      contentType: "text/html; charset=utf-8",
    }),
  );
  await page.route("**/dashboard.js*", (route) =>
    route.fulfill({ path: DASHBOARD_JS_PATH, contentType: "text/javascript; charset=utf-8" }),
  );
  await page.route("**/dashboard/tailwind.css", (route) =>
    route.fulfill({ path: DASHBOARD_CSS_PATH, contentType: "text/css; charset=utf-8" }),
  );

  let submitted = null;
  await page.route("**/api/**", (route) => {
    const request = route.request();
    const url = new URL(request.url());
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = {
        ok: true,
        repositories: [
          {
            owner: "bob",
            name: "proj",
            source: "local-node",
            rootCommit: "same-root",
            forkedFrom: "alice/proj",
            liveHost: true,
            cloneOnline: true,
            updatedAt: "2",
            defaultBranch: "main",
            branches: [{ name: "main" }, { name: "feature" }],
          },
          // Put the fork first deliberately. Its upstream clone identity must
          // not make a lookup for alice/proj resolve this bob/proj row.
          {
            owner: "alice",
            name: "proj",
            source: "local-node",
            rootCommit: "same-root",
            liveHost: false,
            cloneOnline: false,
            updatedAt: "1",
          },
        ],
      };
    } else if (url.pathname === "/api/repo/bob/proj/compare") {
      body = {
        ok: true,
        baseOid: "a".repeat(40),
        headOid: "b".repeat(40),
        patch: "diff --git a/file.txt b/file.txt\n+fork change\n",
        commits: "commit bbbbbbb\nAuthor: Bob\n\n    fork change\n",
      };
    } else if (
      url.pathname === "/api/repo/alice/proj/pulls" &&
      request.method() === "POST"
    ) {
      submitted = request.postDataJSON();
      body = { ok: true, number: 41 };
    } else if (url.pathname.endsWith("/branches")) {
      body = { ok: true, branches: [{ name: "main" }, { name: "feature" }] };
    } else if (url.pathname.endsWith("/tree")) {
      body = { ok: true, entries: [], counts: { openIssues: 0, openPulls: 0 } };
    } else if (url.pathname.endsWith("/forks")) {
      body = { ok: true, count: 0, forks: [] };
    } else if (url.pathname.endsWith("/mirrors")) {
      body = { ok: true, mirrors: [] };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });

  await page.goto("/bob/proj/pulls");
  await page.locator("[data-repo-pull-new]").click();
  await expect(page.locator("[data-repo-pull-new-form]")).toContainText(
    "New pull request to alice/proj",
  );
  await expect(page.locator("[data-repo-pull-new-form]")).toContainText(
    "parent node is offline",
  );
  await page.locator("[data-repo-pull-title]").fill("Change from my fork");
  await page.locator("[data-repo-pull-body]").fill("Portable proposal");
  await page.locator("[data-repo-pull-base]").selectOption("main");
  await page.locator("[data-repo-pull-head]").selectOption("feature");
  await page.locator("[data-repo-pull-submit]").click();

  await expect.poll(() => submitted).not.toBeNull();
  expect(submitted.owner).toBe("alice");
  expect(submitted.repo).toBe("proj");
  expect(submitted.pull.base).toBe("main");
  expect(submitted.pull.head).toBe("bob:feature");
  expect(submitted.pull.patch).toContain("fork change");
  expect(submitted.pull.commits).toContain("commit bbbbbbb");
});

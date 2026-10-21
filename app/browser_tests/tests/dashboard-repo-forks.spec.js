const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const REPO_PAGE_PATH = path.join(PUBLIC, "dashboard", "repo.html");
const DASHBOARD_JS_PATH = path.join(PUBLIC, "dashboard.js");
const DASHBOARD_CSS_PATH = path.join(PUBLIC, "dashboard", "tailwind.css");

// The title row's Fork ▾ shows who forked the repo. The button carries the
// real count from GET /api/repo/o/r/forks and opens a popover listing each fork
// as <owner>/<name>, linking to that fork's own repository page.
test("the fork menu in the title row lists who forked the repo", async ({
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
          queued: false,
          created: true,
          fork: {
            owner: "dana",
            name: "proj",
            url: "/dana/proj",
            forkedFrom: "alice/proj",
            status: "ready",
          },
          push: {
            remoteUrl: "https://forkmesh.test/dana/proj.git",
            username: "dana",
            password: "fmp1_once",
            expiresAt: 4102444800000,
            commands: [
              "git remote add fork https://forkmesh.test/dana/proj.git",
              "git push -u fork main",
            ],
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

  const forkButton = page.locator("[data-repo-title-row] [data-repo-action-fork]");
  await expect(forkButton).toBeVisible();
  await expect(page.locator("[data-repo-about] [data-repo-action-fork]")).toHaveCount(0);
  // Real count, not a placeholder and not a mock number.
  await expect(page.locator("[data-repo-fork-count]")).toHaveText("2");

  const menu = page.locator("[data-repo-fork-menu]");
  await expect(menu).toBeHidden();
  await forkButton.click();
  await expect(menu).toBeVisible();
  await expect(forkButton).toHaveAttribute("aria-expanded", "true");

  // Forking stays entirely on the website: the server makes the copy and
  // hands back a one-time push credential instead of a desktop protocol URL.
  const create = page.locator("[data-repo-fork-create]");
  await expect(create).toHaveAttribute("type", "button");
  await expect(create).toHaveText("Create fork");
  await create.click();
  await expect(create).toContainText("Fork created");
  await expect(page.locator("[data-repo-fork-status]")).toContainText(
    "Your fork dana/proj is ready",
  );
  await expect(page.locator('[data-repo-fork-status] a[href="/dana/proj"]')).toHaveText("Open fork");
  const push = page.locator("[data-repo-fork-push]");
  await expect(push).toBeVisible();
  await expect(push.locator("[data-repo-fork-push-commands]")).toContainText(
    "git remote add fork https://forkmesh.test/dana/proj.git",
  );
  await expect(push.locator("[data-repo-fork-push-username]")).toHaveText("dana");
  await expect(push.locator("[data-repo-fork-push-password]")).toHaveText("fmp1_once");
  await expect(page.locator("[data-repo-fork-primary]")).toHaveAttribute(
    "data-repo-fork-own-url",
    "/dana/proj",
  );
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
  await page.locator("[data-repo-title-visibility]").click();
  await expect(menu).toBeHidden();
  await expect(forkButton).toHaveAttribute("aria-expanded", "false");
});

// GitHub-style title row: logo, name, Public, "forked from" under the name,
// and neutral Fork N ▾ / Star N buttons on the right instead of About rows.
test("the repo title row forks and stars from the header", async ({ page }) => {
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "dana",
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
  await page.route(/https?:\/\/[^/]+\/(?:bob|dana)\/proj$/, (route) =>
    route.fulfill({ path: REPO_PAGE_PATH, contentType: "text/html; charset=utf-8" }),
  );
  await page.route("**/dashboard.js*", (route) =>
    route.fulfill({ path: DASHBOARD_JS_PATH, contentType: "text/javascript; charset=utf-8" }),
  );
  await page.route("**/dashboard/tailwind.css", (route) =>
    route.fulfill({ path: DASHBOARD_CSS_PATH, contentType: "text/css; charset=utf-8" }),
  );

  const starCalls = [];
  let forkPosts = 0;
  let forked = false;
  await page.route("**/api/**", (route) => {
    const request = route.request();
    const url = new URL(request.url());
    let body = { ok: true };
    let status = 200;
    if (url.pathname === "/api/repositories") {
      body = {
        ok: true,
        repositories: [
          {
            owner: "bob",
            name: "proj",
            visibility: "public",
            source: "local-node",
            forkedFrom: "alice/proj",
            rootCommit: "root",
            updatedAt: "2",
          },
        ],
      };
    } else if (url.pathname === "/api/repo/bob/proj/star") {
      starCalls.push(request.method());
      body = request.method() === "POST"
        ? { ok: true, starred: true, count: 4 }
        : { ok: true, starred: false, count: 3 };
    } else if (url.pathname === "/api/repo/bob/proj/forks") {
      if (request.method() === "POST") {
        forkPosts += 1;
        forked = true;
        status = 202;
        body = {
          ok: true,
          created: true,
          queued: false,
          fork: { owner: "dana", name: "proj", url: "/dana/proj", forkedFrom: "bob/proj", status: "ready" },
        };
      } else {
        body = forked
          ? { ok: true, count: 1, forks: [{ owner: "dana", name: "proj", url: "/dana/proj" }] }
          : { ok: true, count: 0, forks: [] };
      }
    }
    return route.fulfill({
      status,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });

  await page.goto("/bob/proj");

  const title = page.locator("[data-repo-title-row]");
  await expect(title).toBeVisible();
  await expect(title.locator("[data-repo-title-name]")).toHaveText("proj");
  await expect(title.locator("[data-repo-title-visibility]")).toHaveText("Public");
  await expect(title.locator('[data-repo-provenance] a[href="/alice/proj"]')).toHaveText("alice/proj");
  await expect(title.locator("[data-repo-provenance]")).toContainText("forked from");
  await expect(title.locator("[data-repo-action-watch]")).toHaveCount(0);
  const tabs = await page.locator("[data-repo-github-header]").boundingBox();
  const row = await title.boundingBox();
  const grid = await page.locator("[data-repo-content-grid]").boundingBox();
  expect(row.y).toBeGreaterThanOrEqual(tabs.y + tabs.height - 1);
  expect(grid.y).toBeGreaterThanOrEqual(row.y + row.height - 1);

  const about = page.locator("[data-repo-about]");
  await expect(about.locator("[data-repo-action-watch]")).toBeVisible();
  await expect(about.locator("[data-repo-action-star]")).toHaveCount(0);
  await expect(about.locator("[data-repo-fork-wrap]")).toHaveCount(0);
  await expect(page.locator("[data-repo-tree-panel]").locator("xpath=preceding-sibling::*[@data-repo-provenance]")).toHaveCount(0);

  const star = title.locator("[data-repo-action-star]");
  await expect(star).toHaveClass(/fm-btn fm-btn-sm/);
  await expect(star.locator("[data-repo-star-label]")).toHaveText("Star");
  await expect(star.locator("[data-repo-star-count]")).toHaveText("3");
  await star.click();
  await expect(star).toHaveAttribute("aria-pressed", "true");
  await expect(star.locator("[data-repo-star-label]")).toHaveText("Starred");
  await expect(star.locator("[data-repo-star-count]")).toHaveText("4");
  expect(starCalls).toContain("POST");

  const primary = title.locator("[data-repo-fork-primary]");
  await expect(primary).toContainText("Fork");
  await expect(title.locator("[data-repo-fork-count]")).toHaveText("0");
  await primary.click();
  await expect(page.locator("[data-repo-fork-menu]")).toBeVisible();
  await expect.poll(() => forkPosts).toBe(1);
  await expect(page.locator("[data-repo-fork-status]")).toContainText("Your fork dana/proj is ready");
  await expect(page.locator("[data-repo-fork-push]")).toBeHidden();
  await expect(title.locator("[data-repo-fork-count]")).toHaveText("1");
  await expect(primary).toHaveAttribute("data-repo-fork-own-url", "/dana/proj");

  await title.locator("[data-repo-title-visibility]").click();
  await expect(page.locator("[data-repo-fork-menu]")).toBeHidden();
  await primary.click();
  await page.waitForURL("**/dana/proj");
  expect(forkPosts).toBe(1);
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

const { test, expect } = require("@playwright/test");
const path = require("node:path");

const REPO_PAGE_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "public",
  "dashboard",
  "repo.html",
);
const DASHBOARD_JS_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "public",
  "dashboard.js",
);
const DASHBOARD_CSS_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "public",
  "dashboard",
  "tailwind.css",
);

test("repo file view saves edits through the file edit endpoint", async ({ page }) => {
  let editRequest = null;
  await page.setViewportSize({ width: 1440, height: 950 });
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.dashboard.theme", "light");
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alice",
      account: "alice",
      sessionToken: "test-session",
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
  await page.route("**/alice/project/**", (route) =>
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
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = {
        ok: true,
        repositories: [{
          owner: "alice",
          name: "project",
          visibility: "public",
          source: "local-node",
          cloneUrl: "https://forkmesh.test/alice/project",
          description: "Editable project",
          defaultBranch: "main",
          branches: ["main", "docs"],
          rootCommit: "0123456789abcdef0123456789abcdef01234567",
          updatedAt: "2",
        }],
      };
    } else if (url.pathname === "/api/repo/alice/project/tree") {
      body = {
        ok: true,
        commit: "0123456789abcdef0123456789abcdef01234567",
        entries: [{
          name: "README.md",
          path: "README.md",
          type: "blob",
          mode: "100644",
          oid: "1111111111111111111111111111111111111111",
          size: 24,
          commit: "0123456789abcdef0123456789abcdef01234567",
          message: "Seed readme",
        }],
        counts: { commits: 1, branches: 2 },
      };
    } else if (url.pathname === "/api/repo/alice/project/blob") {
      body = {
        ok: true,
        commit: "0123456789abcdef0123456789abcdef01234567",
        oid: "1111111111111111111111111111111111111111",
        content: "# Project\n\nInitial text.\n",
      };
    } else if (url.pathname === "/api/repo/alice/project/branches") {
      body = {
        ok: true,
        branches: [{ name: "main" }, { name: "docs" }],
      };
    } else if (url.pathname === "/api/repo/alice/project/about") {
      body = { ok: true };
    } else if (url.pathname === "/api/repo/alice/project/mirrors") {
      body = { ok: true, mirrors: [] };
    } else if (url.pathname === "/api/repo/alice/project/blobs") {
      body = { ok: true, blobs: {} };
    } else if (url.pathname === "/api/repo/alice/project/file-edit") {
      editRequest = {
        authorization: request.headers().authorization || "",
        body: request.postDataJSON(),
      };
      body = {
        ok: true,
        path: "README.md",
        branch: "web/readme",
        commit: "abcdefabcdefabcdefabcdefabcdefabcdefabcd",
        oid: "2222222222222222222222222222222222222222",
      };
    }
    await route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });

  await page.goto("/alice/project/blob/README.md");
  await expect(page.getByRole("article")).toContainText("Initial text.");
  await page.locator('[data-repo-file-action="edit"]').click();
  const editor = page.locator("[data-repo-file-editor]");
  await expect(editor).toBeVisible();
  await editor.locator("[data-repo-file-editor-content]").fill(
    "# Project\n\nEdited in the browser.\n",
  );
  await editor.locator("[data-repo-file-editor-branch]").fill("web/readme");
  await editor.locator("[data-repo-file-editor-message]").fill("Edit README from web");
  await editor.locator("[data-repo-file-editor-save]").click();

  await expect(page.locator("[data-repo-file-status]")).toContainText(
    "Saved to web/readme at abcdefabcd.",
  );
  expect(editRequest.authorization).toBe("Bearer test-session");
  expect(editRequest.body).toMatchObject({
    path: "README.md",
    branch: "web/readme",
    baseRef: "main",
    oid: "1111111111111111111111111111111111111111",
    content: "# Project\n\nEdited in the browser.\n",
    message: "Edit README from web",
    sessionToken: "test-session",
  });
});

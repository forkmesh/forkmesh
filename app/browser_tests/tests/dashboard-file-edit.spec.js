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

const PROPOSAL_PATCH = [
  "diff --git a/README.md b/README.md",
  "index 1111111111111111111111111111111111111111..2222222222222222222222222222222222222222 100644",
  "--- a/README.md",
  "+++ b/README.md",
  "@@ -1,3 +1,3 @@",
  " # Project",
  " ",
  "-Initial text.",
  "+Suggested text.",
  "",
].join("\n");

async function openRepoFile(page, { account, onFileEdit, onPulls }) {
  await page.setViewportSize({ width: 1440, height: 950 });
  await page.addInitScript((name) => {
    localStorage.setItem("forkmesh.dashboard.theme", "light");
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: name,
      account: name,
      sessionToken: "test-session",
    }));
  }, account);
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
      body = onFileEdit({
        authorization: request.headers().authorization || "",
        body: request.postDataJSON(),
      });
    } else if (url.pathname === "/api/repo/alice/project/pulls"
        && request.method() === "POST" && onPulls) {
      body = onPulls(request.postDataJSON());
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
  return editor;
}

test("repo file view saves edits through the file edit endpoint", async ({ page }) => {
  const requests = [];
  const editor = await openRepoFile(page, {
    account: "alice",
    onFileEdit(request) {
      requests.push(request);
      if (request.body.mode === "probe") {
        return { ok: true, access: "write", account: "alice" };
      }
      return {
        ok: true,
        path: "README.md",
        branch: "web/readme",
        commit: "abcdefabcdefabcdefabcdefabcdefabcdefabcd",
        oid: "2222222222222222222222222222222222222222",
      };
    },
  });
  await expect(editor.locator("[data-repo-file-editor-propose-wrap]")).toBeVisible();
  await expect(editor.locator("[data-repo-file-editor-notice]")).toBeHidden();
  await editor.locator("[data-repo-file-editor-content]").fill(
    "# Project\n\nEdited in the browser.\n",
  );
  await editor.locator("[data-repo-file-editor-branch]").fill("web/readme");
  await editor.locator("[data-repo-file-editor-message]").fill("Edit README from web");
  await editor.locator("[data-repo-file-editor-save]").click();

  await expect(page.locator("[data-repo-file-status]")).toContainText(
    "Saved to web/readme at abcdefabcd.",
  );
  const save = requests.find((request) => request.body.mode === "commit");
  expect(save.authorization).toBe("Bearer test-session");
  expect(save.body).toMatchObject({
    mode: "commit",
    path: "README.md",
    branch: "web/readme",
    baseRef: "main",
    oid: "1111111111111111111111111111111111111111",
    content: "# Project\n\nEdited in the browser.\n",
    message: "Edit README from web",
    sessionToken: "test-session",
  });
});

test("a reader without write access proposes the edit as a pull request", async ({ page }) => {
  const requests = [];
  let submitted = null;
  const editor = await openRepoFile(page, {
    account: "jett",
    onFileEdit(request) {
      requests.push(request);
      if (request.body.mode === "probe") {
        return { ok: true, access: "propose", account: "jett" };
      }
      return {
        ok: true,
        proposal: true,
        path: "README.md",
        base: "main",
        baseOid: "0123456789abcdef0123456789abcdef01234567",
        head: "jett/edit-readme-md-abcdefa",
        headOid: "abcdefabcdefabcdefabcdefabcdefabcdefabcd",
        oid: "2222222222222222222222222222222222222222",
        author: "jett",
        subject: "Suggest README wording",
        patch: PROPOSAL_PATCH,
        commits: "From abcdef Mon Sep 17 00:00:00 2001\nSubject: [PATCH] Suggest README wording\n\n---\n\n" + PROPOSAL_PATCH,
      };
    },
    onPulls(payload) {
      submitted = payload;
      return { ok: true, number: 7 };
    },
  });
  const notice = editor.locator("[data-repo-file-editor-notice]");
  await expect(notice).toBeVisible();
  await expect(notice).toContainText("You don't have write access");
  await expect(editor.locator("[data-repo-file-editor-propose-wrap]")).toBeHidden();
  await expect(editor.locator("[data-repo-file-editor-save]")).toContainText("Propose changes");
  await editor.locator("[data-repo-file-editor-content]").fill(
    "# Project\n\nSuggested text.\n",
  );
  await editor.locator("[data-repo-file-editor-message]").fill("Suggest README wording");
  await editor.locator("[data-repo-file-editor-save]").click();

  await page.waitForURL("**/alice/project/pulls/7");
  const propose = requests.find((request) => request.body.mode === "propose");
  expect(propose.body).toMatchObject({
    mode: "propose",
    path: "README.md",
    branch: "main",
    content: "# Project\n\nSuggested text.\n",
    message: "Suggest README wording",
  });
  expect(requests.some((request) => request.body.mode === "commit")).toBe(false);
  expect(submitted.pull).toMatchObject({
    title: "Suggest README wording",
    base: "main",
    head: "jett/edit-readme-md-abcdefa",
    patch: PROPOSAL_PATCH,
  });
  expect(submitted.pull.sig).toBeTruthy();
});

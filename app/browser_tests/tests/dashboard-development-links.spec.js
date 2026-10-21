const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const REPO_PAGE_PATH = path.join(PUBLIC, "dashboard", "repo.html");
const DASHBOARD_JS_PATH = path.join(PUBLIC, "dashboard.js");
const DASHBOARD_CSS_PATH = path.join(PUBLIC, "dashboard", "tailwind.css");
const MAIN_COMMIT = "2222222222222222222222222222222222222222";
const PULL_COMMIT = "1111111111111111111111111111111111111111";
const OPENED_AT = 1_795_000_000_000;

function issueJson(number, title, comments = []) {
  return JSON.stringify({
    number,
    title,
    status: "open",
    createdAt: OPENED_AT,
    events: [
      { id: `open-${number}`, type: "open", ts: OPENED_AT, author: "jett-key", authorName: "jett", title, body: "" },
      ...comments.map((body, index) => ({
        id: `comment-${number}-${index}`, type: "comment", ts: OPENED_AT + index + 1,
        author: "dana-key", authorName: "dana", body,
      })),
    ],
  });
}

function pullMd(number, title, body) {
  return `---
schema: forkmesh-pull-v1
number: ${number}
title: ${title}
base: main
head: dana:topic-${number}
status: open
ts: ${OPENED_AT}
author: dana-key
authorName: dana
---

${body}
`;
}

const BLOBS = {
  ".forkmesh/issues/open/7/issue-7.json": issueJson(7, "Login button does nothing"),
  ".forkmesh/issues/open/8/issue-8.json": issueJson(8, "Search is slow", ["Linked pull request #3"]),
  ".forkmesh/issues/open/9/issue-9.json": issueJson(9, "Header overlaps"),
  ".forkmesh/pulls/3/pull.md": pullMd(3, "Fix login", "Fixes #7"),
  ".forkmesh/pulls/4/pull.md": pullMd(4, "Add search index", "Faster lookups."),
  ".forkmesh/pulls/3/0001-comment.md": `---
type: comment
id: comment-pull-3
author: dana-key
authorName: dana
ts: 1795000000
---

This also resolves #9.
`,
};

async function mockRepo(page, posted) {
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "dana", sessionToken: "test-session", status: "active",
    }));
  });
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.tailwind = {};" }));
  await page.route("https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.lucide = { createIcons() {} };" }));
  await page.route("**/dana/notes/**", (route) =>
    route.fulfill({ path: REPO_PAGE_PATH, contentType: "text/html; charset=utf-8" }));
  await page.route("**/dashboard.js*", (route) =>
    route.fulfill({ path: DASHBOARD_JS_PATH, contentType: "text/javascript; charset=utf-8" }));
  await page.route("**/dashboard/tailwind.css", (route) =>
    route.fulfill({ path: DASHBOARD_CSS_PATH, contentType: "text/css; charset=utf-8" }));
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    const target = url.searchParams.get("path") || "";
    const json = (body, status = 200) => route.fulfill({
      status, contentType: "application/json; charset=utf-8", body: JSON.stringify(body),
    });
    if (url.pathname === "/api/repositories") {
      return json({ ok: true, repositories: [{
        owner: "dana", name: "notes", visibility: "public", source: "local-node",
        cloneUrl: "https://forkmesh.test/dana/notes", rootCommit: "root", stateHash: "state", updatedAt: "2",
      }] });
    }
    if (url.pathname === "/api/repo/dana/notes/branches") {
      return json({ ok: true, branches: [{ name: "main", commit: MAIN_COMMIT }] });
    }
    if (url.pathname === "/api/repo/dana/notes/blob") {
      return BLOBS[target] ? json({ ok: true, content: BLOBS[target] }) : json({ ok: false, error: "not_found" }, 404);
    }
    if (url.pathname === "/api/repo/dana/notes/blobs") {
      return json({ ok: true, blobs: Object.fromEntries(url.searchParams.getAll("path")
        .filter((name) => BLOBS[name]).map((name) => [name, { content: BLOBS[name] }])) });
    }
    if (url.pathname === "/api/repo/dana/notes/tree") {
      const entries = {
        ".forkmesh/issues/open": ["7", "8", "9"].map((name) => ({ type: "tree", name })),
        ".forkmesh/pulls": ["3", "4"].map((name) => ({ type: "tree", name })),
        ".forkmesh/pulls/3": [{ type: "blob", name: "0001-comment.md" }],
      }[target] || [];
      return json({ ok: true, commit: target.startsWith(".forkmesh/pulls") ? PULL_COMMIT : MAIN_COMMIT, entries });
    }
    if (route.request().method() === "POST" && /\/api\/repo\/dana\/notes\/(issues|pulls)$/.test(url.pathname)) {
      posted.push({ kind: url.pathname.split("/").pop(), ...route.request().postDataJSON() });
    }
    return json({ ok: true });
  });
}

const issuePosts = (posted) => posted.filter((entry) => entry.kind === "issues")
  .map((entry) => [entry.number, entry.event.body]);

test("an issue links a pull request from its Development section", async ({ page }) => {
  const posted = [];
  await mockRepo(page, posted);
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto("/dana/notes/issues/7");

  const development = page.locator('[data-repo-development="issues"]');
  await expect(development.locator('a[href$="/pulls/3"]')).toContainText("Fix login", { timeout: 15_000 });

  await page.locator('[data-repo-issue-picker-open="development"]').click();
  const picker = page.locator('[data-repo-issue-picker="development"]');
  await expect(picker).toContainText("Link a pull request");
  await picker.locator("[data-repo-issue-picker-search]").fill("search");
  await expect(picker.locator("[data-repo-issue-picker-option]")).toHaveCount(1);
  await picker.locator('[data-repo-issue-picker-option="4"]').click();
  await page.keyboard.press("Escape");

  await expect(picker).toHaveCount(0);
  await expect(page.locator('[data-repo-issue-picker-open="development"]')).toBeFocused();

  await expect.poll(() => issuePosts(posted)).toEqual([[7, "Linked pull request #4"]]);
  await expect(development.locator('a[href$="/pulls/4"]')).toContainText("Add search index");
  await expect(development.locator('a[href$="/pulls/3"]')).toBeVisible();
  await expect(page.locator("[data-repo-issue-development-event]")).toContainText("linked pull request");
  await expect(page.locator('[data-repo-issue-development-event] a[href$="/pulls/4"]')).toHaveText("#4");
});

test("a pull request links issues from its Development section and its comments", async ({ page }) => {
  const posted = [];
  await mockRepo(page, posted);
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto("/dana/notes/pulls/3");

  const development = page.locator('[data-repo-development="pulls"]');
  for (const number of [7, 8, 9]) {
    await expect(development.locator(`a[href$="/issues/${number}"]`)).toBeVisible({ timeout: 15_000 });
  }

  await page.locator('[data-repo-issue-picker-open="development"]').click();
  const picker = page.locator('[data-repo-issue-picker="development"]');
  await expect(picker).toContainText("Link an issue");
  await expect(picker.locator('[data-repo-issue-picker-option="8"]')).toHaveAttribute("aria-selected", "true");
  await picker.locator('[data-repo-issue-picker-option="8"]').click();
  await page.keyboard.press("Escape");

  await expect.poll(() => issuePosts(posted)).toEqual([[8, "Unlinked pull request #3"]]);
  await expect(development.locator('a[href$="/issues/8"]')).toHaveCount(0);
  await expect(development.locator('a[href$="/issues/7"]')).toBeVisible();

  await page.locator("[data-repo-pull-review-body]").fill("Now this fixes #8 as well.");
  await page.locator('[data-repo-pull-review-action="comment"]').click();
  await expect.poll(() => issuePosts(posted)).toEqual([
    [8, "Unlinked pull request #3"],
    [8, "Linked pull request #3"],
  ]);
  await expect(development.locator('a[href$="/issues/8"]')).toBeVisible();
});

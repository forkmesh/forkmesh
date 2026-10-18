const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const REPO_PAGE_PATH = path.join(PUBLIC, "dashboard", "repo.html");
const DASHBOARD_JS_PATH = path.join(PUBLIC, "dashboard.js");
const DASHBOARD_CSS_PATH = path.join(PUBLIC, "dashboard", "tailwind.css");
const COMMIT = "0123456789abcdef0123456789abcdef01234567";
const SVG = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 8"><rect width="8" height="8" fill="#16a34a"/></svg>';

function entry(name, type) {
  return { name, path: name, type, mode: type === "tree" ? "040000" : "100644", oid: "1".repeat(40), size: 10, message: "seed" };
}

const TREES = {
  "": [entry("bolt-c", "tree"), entry("docs", "tree"), entry("README.md", "blob")],
  "bolt-c": [entry("src", "tree"), entry("README.md", "blob"), entry("logo.svg", "blob"), entry("logo.png", "blob")],
  docs: [entry("guide.md", "blob")],
};

const LEAF_ABOUT = {
  path: "bolt-c",
  source: "project",
  invalid: false,
  name: "Bolt-C",
  description: "A simple one-page application to track <b>pull request</b> readiness.",
  descriptionSource: "readme",
  website: "https://leaf.example.net/",
  websiteSource: "readme",
  links: [
    { label: "Docs", url: "https://docs.example.test/leaf" },
    { label: "Bad", url: "javascript:alert(1)" },
  ],
  topics: ["security", "github"],
  logo: { path: "bolt-c/logo.svg", type: "image/svg+xml", url: `/api/repo/alice/project/dir-logo?path=bolt-c&ref=${COMMIT}` },
  readme: "bolt-c/README.md",
};

async function openRepo(page, address, requests) {
  await page.setViewportSize({ width: 1440, height: 950 });
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.dashboard.theme", "light");
  });
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.tailwind = {};" }));
  await page.route("https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.lucide = { createIcons() {} };" }));
  await page.route("**/alice/project**", (route) =>
    route.fulfill({ path: REPO_PAGE_PATH, contentType: "text/html; charset=utf-8" }));
  await page.route("**/dashboard.js*", (route) =>
    route.fulfill({ path: DASHBOARD_JS_PATH, contentType: "text/javascript; charset=utf-8" }));
  await page.route("**/dashboard/tailwind.css", (route) =>
    route.fulfill({ path: DASHBOARD_CSS_PATH, contentType: "text/css; charset=utf-8" }));
  await page.route("**/api/**", async (route) => {
    const url = new URL(route.request().url());
    requests.push(url);
    if (url.pathname === "/api/repo/alice/project/dir-logo") {
      await route.fulfill({ status: 200, contentType: "image/svg+xml", body: SVG });
      return;
    }
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = {
        ok: true,
        repositories: [{
          owner: "alice", name: "project", visibility: "public", source: "local-node",
          cloneUrl: "https://forkmesh.test/alice/project", description: "Monorepo",
          defaultBranch: "main", branches: ["main"], rootCommit: COMMIT, updatedAt: "2",
        }],
      };
    } else if (url.pathname === "/api/repo/alice/project/tree") {
      const treePath = url.searchParams.get("path") || "";
      body = { ok: true, commit: COMMIT, entries: TREES[treePath] || [] };
    } else if (url.pathname === "/api/repo/alice/project/dir-about") {
      const dir = url.searchParams.get("path");
      body = { ok: true, path: dir, commit: COMMIT, about: dir === "bolt-c" ? LEAF_ABOUT : null };
    } else if (url.pathname === "/api/repo/alice/project/blob") {
      body = { ok: true, commit: COMMIT, content: "# Project\n\nReadme.\n" };
    } else if (url.pathname === "/api/repo/alice/project/branches") {
      body = { ok: true, branches: [{ name: "main" }] };
    } else if (url.pathname === "/api/repo/alice/project/mirrors") {
      body = { ok: true, mirrors: [] };
    } else if (url.pathname === "/api/repo/alice/project/blobs") {
      body = { ok: true, blobs: {} };
    }
    await route.fulfill({ status: 200, contentType: "application/json; charset=utf-8", body: JSON.stringify(body) });
  });
  await page.goto(address);
}

test("a folder with an About shows it in the right-hand rail with its logo", async ({ page }) => {
  const requests = [];
  await openRepo(page, "/alice/project/tree/bolt-c", requests);
  const aside = page.locator("[data-repo-dir-about]");
  await expect(aside).toBeVisible();
  await expect(page.locator("[data-repo-about]")).toBeHidden();
  await expect(aside.locator("[data-repo-dir-about-name]")).toHaveText("Bolt-C");
  await expect(aside.locator("[data-repo-dir-about-description]")).toHaveText(LEAF_ABOUT.description);
  await expect(aside.locator("[data-repo-dir-about-description] b")).toHaveCount(0);
  await expect(aside.locator("[data-repo-dir-about-topic]")).toHaveText(["security", "github"]);
  await expect(aside.locator('a[href="https://leaf.example.net/"]')).toHaveAttribute("rel", /noopener/);
  await expect(aside.locator('a[href="https://docs.example.test/leaf"]')).toHaveCount(1);
  await expect(aside.locator('a[href^="javascript:"]')).toHaveCount(0);
  await expect(aside.locator('[data-dashboard-blob-path="bolt-c/README.md"]')).toHaveCount(1);
  await expect(aside.locator("[data-repo-dir-about-logo]")).toHaveAttribute("src", LEAF_ABOUT.logo.url);
  const headerLogo = page.locator("[data-repo-dir-logo]");
  await expect(headerLogo).toBeVisible();
  await expect(headerLogo).toHaveAttribute("src", LEAF_ABOUT.logo.url);
  const call = requests.find((url) => url.pathname.endsWith("/dir-about"));
  expect(call.searchParams.get("path")).toBe("bolt-c");
  expect(call.searchParams.get("ref")).toBe(COMMIT);

  await page.goto("/alice/project/tree/docs");
  await expect(page.locator("[data-repo-tree] [data-dashboard-blob-path=\"docs/guide.md\"]")).toBeVisible();
  await expect(page.locator("[data-repo-dir-about]")).toBeHidden();
  await expect(page.locator("[data-repo-dir-logo]")).toBeHidden();
});

test("a folder without an about file or logo does not ask for one", async ({ page }) => {
  const requests = [];
  await openRepo(page, "/alice/project/tree/docs", requests);
  await expect(page.locator("[data-repo-tree] [data-dashboard-blob-path=\"docs/guide.md\"]")).toBeVisible();
  await expect(page.locator("[data-repo-dir-about]")).toBeHidden();
  expect(requests.some((url) => url.pathname.endsWith("/dir-about"))).toBe(false);
});

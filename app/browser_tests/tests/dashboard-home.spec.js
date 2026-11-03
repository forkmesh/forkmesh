const { test, expect } = require("@playwright/test");
const path = require("node:path");

const HOME_PAGE_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "public",
  "dashboard",
  "index.html",
);

function repositories(count) {
  return Array.from({ length: count }, (_, index) => ({
    owner: "alice",
    name: `repo-${index}`,
    description: index === 0 ? "The first repository, with a description long enough to wrap." : "",
    visibility: "public",
    source: "local-node",
    liveHost: true,
    cloneUrl: `https://forkmesh.test/alice/repo-${index}`,
    rootCommit: `root-${index}`,
    stateHash: `state-${index}`,
    updatedAt: String(index + 1),
  }));
}

const NOTIFICATIONS = [
  { id: "n1", kind: "mention", title: "bob mentioned you", actor: "bob", repo: "alice/repo-0", ts: 1 },
  { id: "n2", kind: "pull_submitted", title: "Pull request submitted", actor: "@carol", repo: "alice/repo-1", ts: 2 },
  { id: "n3", kind: "code_push", title: "You pushed to main", actor: "alice", repo: "alice/repo-0", ts: 3 },
  { id: "n4", kind: "operational_alert", title: "Website recovered", ts: 4 },
];

const AGENT_SESSIONS = {
  ok: true,
  account: "alice",
  activeCount: 1,
  sessions: [
    { id: "s1", repo: "repo-0", provider: "claude-code", status: "running", title: "Fix the README", updatedAt: Date.now() },
    { id: "s2", org: "acme", repo: "site", provider: "codex", status: "completed", title: "", issueNumber: 7, updatedAt: Date.now() - 3_600_000 },
  ],
};

const USER_SESSION = {
  nodeName: "alice",
  email: "alice@example.test",
  emailVerified: true,
  sessionToken: "test-session",
  status: "active",
};

async function openHome(page, { repos = repositories(3), notifications = NOTIFICATIONS, session = USER_SESSION, agents = AGENT_SESSIONS, repoApi = () => null } = {}) {
  await page.addInitScript((stored) => {
    localStorage.setItem("forkmesh.session", JSON.stringify(stored));
  }, session);
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
  await page.route("**/dashboard", (route) => route.fulfill({
    path: HOME_PAGE_PATH,
    contentType: "text/html; charset=utf-8",
  }));
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = { ok: true, repositories: repos };
    } else if (url.pathname === "/api/notifications") {
      body = { ok: true, notifications, unread: notifications.length };
    } else if (url.pathname === "/api/account/agent-sessions") {
      body = agents;
    } else if (url.pathname.startsWith("/api/repo/")) {
      body = repoApi(url) || body;
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.goto("/dashboard");
}

test("home is the prompt composer followed by the account's agents", async ({ page }) => {
  await openHome(page);

  const composer = page.locator("[data-composer-slot] [data-composer]");
  await expect(composer).toHaveAttribute("data-inline", "1");
  await expect(composer.locator("[data-composer-panel]")).toBeVisible();
  await expect(composer.locator("[data-composer-pill]")).toBeHidden();
  await expect(page.locator("body > [data-composer]")).toHaveCount(0);
  await expect(page.locator("[data-home-agent-sample]")).toHaveCount(0);

  await page.keyboard.press("Escape");
  await expect(composer.locator("[data-composer-panel]")).toBeVisible();

  const agents = page.locator("[data-home-agents] [data-home-agent]");
  await expect(agents).toHaveCount(2);
  await expect(agents.nth(0)).toContainText("Fix the README");
  await expect(agents.nth(0)).toHaveAttribute("href", "/alice/repo-0/agents");
  await expect(agents.nth(1)).toContainText("Issue #7");
  await expect(agents.nth(1)).toHaveAttribute("href", "/acme/site/agents");
  await expect(page.locator("[data-home-agents-count]")).toHaveText("1 running");
});

test("home mounts the composer for an account whose session has no email", async ({ page }) => {
  await openHome(page, { session: { ...USER_SESSION, email: "" } });

  await expect(page.locator("[data-composer-slot] [data-composer]")).toHaveAttribute("data-inline", "1");
  await expect(page.locator("[data-composer-slot]")).not.toContainText("Loading prompt");
});

test("home hides the prompt slot when the composer does not mount", async ({ page }) => {
  await openHome(page, { session: { ...USER_SESSION, email: "", kind: "node" } });

  await expect(page.locator("[data-home-agents-count]")).toBeAttached();
  await expect(page.locator("[data-composer-slot]")).toBeHidden();
  await expect(page.locator("[data-composer]")).toHaveCount(0);
});

test("the Feed tab shows only other people's activity", async ({ page }) => {
  await openHome(page);

  await expect(page.locator('[data-home-tab-panel="feed"]')).toBeHidden();
  await page.locator('[data-home-left-rail] [data-home-tab="feed"]').click();
  await expect(page.locator('[data-home-tab-panel="home"]')).toBeHidden();
  const feed = page.locator('[data-home-tab-panel="feed"] [data-home-feed]');
  await expect(feed).toBeVisible();
  const cards = feed.locator('[data-home-feed-kind="activity"]');
  await expect(cards).toHaveCount(2);
  await expect(feed).toContainText("bob mentioned you");
  await expect(feed).toContainText("@carol");
  await expect(feed).not.toContainText("You pushed to main");
  await expect(feed).not.toContainText("Website recovered");
  await expect(page.locator('[data-home-feed-kind="repository"]')).toHaveCount(0);
});

test("repositories live in the rail list and the finder hides below ten", async ({ page }) => {
  await openHome(page);

  const rows = page.locator("[data-home-top-repositories] [data-home-repo-row]");
  await expect(rows).toHaveCount(3);
  await expect(rows.first()).toHaveAttribute("title", /The first repository/);
  await expect(rows.first()).not.toContainText("The first repository");
  await expect(rows.first().locator("[data-home-repo-served-by]")).toHaveAttribute("title", "live host");
  await expect(page.locator("[data-home-repo-search-toggle]")).toBeHidden();
  await expect(page.locator("[data-home-repo-search-field]")).toBeHidden();
});

test("the finder shows once there are ten repositories", async ({ page }) => {
  await openHome(page, { repos: repositories(10) });

  await expect(page.locator("[data-home-top-repositories] [data-home-repo-row]")).toHaveCount(8);
  const search = page.locator("[data-home-repo-search]");
  await expect(search).toBeHidden();
  await page.locator("[data-home-repo-search-toggle]").click();
  await expect(search).toBeVisible();
  await expect(search).toBeFocused();
  await search.fill("repo-9");
  await expect(page.locator("[data-home-top-repositories] [data-home-repo-row]")).toHaveCount(1);
});


test("home greets the account and says when there are no agent sessions", async ({ page }) => {
  await openHome(page, { agents: { ok: true, account: "alice", activeCount: 0, sessions: [] } });

  await expect(page.locator("[data-home-greeting]")).toHaveText(/^Good (morning|afternoon|evening), alice!$/);
  await expect(page.locator("[data-home-account-name]")).toHaveText("alice");
  await expect(page.locator("[data-home-agents]")).toHaveText("No agent sessions.");
  await expect(page.locator("[data-home-pulls-section]")).toBeHidden();
  await expect(page.locator("[data-home-issues-section]")).toBeHidden();
});

test("home lists the open pull requests and issues in the account's repositories", async ({ page }) => {
  const now = Date.now();
  const pull = (number, title, status) => ({
    content: `---\ntitle: ${title}\nstatus: ${status}\nauthor: bob\nauthorName: bob\nupdatedAt: ${now - number * 1000}\n---\nBody`,
  });
  await openHome(page, {
    repos: repositories(2).map((repo, index) => ({ ...repo, openPulls: index === 0 ? 2 : 0, openIssues: index === 0 ? 1 : 0 })),
    repoApi(url) {
      if (!url.pathname.startsWith("/api/repo/alice/repo-0/")) return null;
      const action = url.pathname.split("/").pop();
      if (action === "tree") {
        return { ok: true, commit: "a".repeat(40), entries: [{ type: "tree", name: "2" }, { type: "tree", name: "1" }] };
      }
      if (action === "blobs") {
        return {
          ok: true,
          blobs: {
            ".forkmesh/pulls/2/pull.md": pull(2, "Open change", "open"),
            ".forkmesh/pulls/1/pull.md": pull(1, "Merged change", "merged"),
          },
        };
      }
      if (action === "issue-index") {
        return {
          ok: true,
          issues: [
            { number: 5, title: "Open bug", status: "open", author: "carol", dateValue: now, commentCount: 3 },
            { number: 4, title: "Closed bug", status: "closed", author: "carol", dateValue: now },
          ],
        };
      }
      return null;
    },
  });

  const pulls = page.locator('[data-home-pulls] [data-home-work-row="pull"]');
  await expect(pulls).toHaveCount(1);
  await expect(pulls.first()).toContainText("Open change");
  await expect(pulls.first()).toHaveAttribute("href", "/alice/repo-0/pull/2");
  await expect(page.locator("[data-home-pulls-count]")).toHaveText("1 open pull request");

  const issues = page.locator('[data-home-issues] [data-home-work-row="issue"]');
  await expect(issues).toHaveCount(1);
  await expect(issues.first()).toContainText("Open bug");
  await expect(issues.first()).toHaveAttribute("href", "/alice/repo-0/issues/5");
});

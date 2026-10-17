const { test, expect } = require("@playwright/test");
const path = require("node:path");

const NOTES_PAGE_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "public",
  "dashboard",
  "notes",
  "index.html",
);

const FIRST_ID = "a".repeat(32);
const SECOND_ID = "b".repeat(32);

function noteFor(id, shares) {
  return {
    id,
    title: id === FIRST_ID ? "Roadmap" : "Retro",
    markdown: id === FIRST_ID ? "Hello" : "Sprint lessons learned",
    version: 1,
    role: "owner",
    visibility: id === FIRST_ID ? "public" : "private",
    updatedAt: new Date().toISOString(),
    shares,
    links: [],
  };
}

async function openNotes(page, { search = "", userQueries = [], shareRequests = [], calls = [] } = {}) {
  const shares = { [FIRST_ID]: [], [SECOND_ID]: [{ type: "user", name: "bob", role: "editor" }] };
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alice",
      email: "alice@example.test",
      emailVerified: true,
      sessionToken: "test-session",
      status: "active",
    }));
    window.ForkMeshDashboard = {
      _registrations: [["notes", ({ builtinPage }) => builtinPage("notes")]],
      registerPage(pageId, initializer) { this._registrations.push([pageId, initializer]); },
    };
    window.__copied = [];
    Object.defineProperty(navigator, "clipboard", {
      configurable: true,
      value: { writeText: async (text) => { window.__copied.push(text); } },
    });
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
  await page.route(/\/dashboard\/notes(\?.*)?$/, (route) => route.fulfill({
    path: NOTES_PAGE_PATH,
    contentType: "text/html; charset=utf-8",
  }));
  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const single = url.pathname.match(/^\/api\/notes\/([0-9a-f]{32})(\/shares)?$/);
    let body = { ok: true };
    calls.push({ method: request.method(), path: url.pathname, query: url.searchParams.get("query"), body: request.postDataJSON() });
    if (url.pathname === "/api/notes") {
      const query = (url.searchParams.get("query") || "").toLowerCase();
      body = { ok: true, notes: [noteFor(FIRST_ID, shares[FIRST_ID]), noteFor(SECOND_ID, shares[SECOND_ID])]
        .filter((note) => `${note.title}\n${note.markdown}`.toLowerCase().includes(query))
        .map(({ markdown, ...note }) => note) };
    } else if (url.pathname === "/api/tasks") {
      body = { ok: true, task: { id: "task-1" } };
    } else if (url.pathname === "/api/repo/alice/project/issues") {
      body = { ok: true, number: 7 };
    } else if (single && single[2]) {
      const payload = request.postDataJSON();
      shareRequests.push(payload);
      shares[single[1]] = [...shares[single[1]].filter((share) => share.name !== payload.name), payload];
      body = { ok: true, shares: shares[single[1]] };
    } else if (single) {
      body = { ok: true, note: noteFor(single[1], shares[single[1]]) };
    } else if (url.pathname === "/api/chat/direct-messages/users") {
      const query = url.searchParams.get("query");
      userQueries.push(query);
      body = { ok: true, users: ["bob", "bobby", "carol"].filter((name) => name.includes(query)).map((name) => ({ name })) };
    } else if (url.pathname === "/api/orgs") {
      body = { ok: true, orgs: [{ name: "bobcats", role: "member" }, { name: "acme", role: "owner" }] };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.goto(`/dashboard/notes${search}`);
}

test("the share field suggests users and organizations and picks one", async ({ page }) => {
  const userQueries = [];
  const shareRequests = [];
  await openNotes(page, { userQueries, shareRequests });
  await expect(page.locator("[data-note-title]")).toHaveValue("Roadmap");
  await page.locator("[data-note-share-open]").click();
  const input = page.locator("[data-note-share-name]");
  await input.fill("bob");
  const options = page.locator("[data-note-share-option]");
  await expect(options).toHaveCount(3);
  await expect(options.nth(0)).toContainText("bob");
  await expect(options.nth(2)).toContainText("bobcats");
  await expect(options.nth(2)).toContainText("Organization");
  await expect(input).toHaveAttribute("aria-expanded", "true");
  expect(userQueries).toContain("bob");

  await input.press("ArrowDown");
  await input.press("ArrowDown");
  await expect(options.nth(2)).toHaveAttribute("aria-selected", "true");
  await input.press("Enter");
  await expect(input).toHaveValue("bobcats");
  await expect(page.locator("[data-note-share-type]")).toHaveValue("organization");
  await expect(page.locator("[data-note-share-suggestions]")).toBeHidden();
  expect(shareRequests).toHaveLength(0);

  await input.press("Enter");
  await expect.poll(() => shareRequests.length).toBe(1);
  expect(shareRequests[0]).toMatchObject({ type: "organization", name: "bobcats" });
  await expect(page.locator("[data-note-shares]")).toContainText("bobcats");

  await input.fill("bob");
  await expect(options).toHaveCount(2);
  await expect(page.locator("[data-note-share-suggestions]")).not.toContainText("bobcats");
  await options.nth(1).click();
  await expect(input).toHaveValue("bobby");
  await expect(page.locator("[data-note-share-type]")).toHaveValue("user");
});

test("a shared note offers a link that opens it for the people it is shared with", async ({ page }) => {
  await openNotes(page);
  await expect(page.locator("[data-note-title]")).toHaveValue("Roadmap");
  await page.locator("[data-note-share-open]").click();
  await expect(page.locator("[data-note-access-row]")).toBeHidden();
  await page.locator("[data-note-share-name]").fill("carol");
  await page.locator("[data-note-share-add]").click();
  await expect(page.locator("[data-note-access-row]")).toBeVisible();
  const link = `/dashboard/notes?note=${FIRST_ID}`;
  await expect(page.locator("[data-note-access-link]")).toHaveAttribute("href", new RegExp(`${link.replace("?", "\\?")}$`));
  await page.locator("[data-note-copy-access-link]").click();
  await expect(page.locator('[data-note-notice="share"]')).toHaveText("Link copied");
  expect(await page.evaluate(() => window.__copied.at(-1))).toMatch(new RegExp(`${link.replace("?", "\\?")}$`));
  expect(new URL(page.url()).searchParams.get("note")).toBe(FIRST_ID);
});

test("opening the link selects the note it names", async ({ page }) => {
  await openNotes(page, { search: `?note=${SECOND_ID}` });
  await expect(page.locator("[data-note-title]")).toHaveValue("Retro");
  await page.locator("[data-note-menu-open]").click();
  await page.locator("[data-note-menu-copy-link]").click();
  await expect(page.locator('[data-note-notice="main"]')).toHaveText("Link copied");
  expect(await page.evaluate(() => window.__copied.at(-1))).toMatch(new RegExp(`/dashboard/notes\\?note=${SECOND_ID}$`));
});

test("searching notes matches the body as well as the title", async ({ page }) => {
  const calls = [];
  await openNotes(page, { calls });
  await expect(page.locator("[data-note-title]")).toHaveValue("Roadmap");
  await page.locator("[data-note-search]").fill("lessons");
  await expect(page.locator("[data-note-list] [data-note-id]")).toHaveCount(1);
  await expect(page.locator("[data-note-list]")).toContainText("Retro");
  expect(calls.some((call) => call.path === "/api/notes" && call.query === "lessons")).toBe(true);
  await page.locator("[data-note-search]").fill("nothing like this");
  await expect(page.locator("[data-note-list]")).toContainText("No notes match");
});

test("a note turns into a task with its Markdown and published link", async ({ page }) => {
  const calls = [];
  await openNotes(page, { calls });
  await expect(page.locator("[data-note-title]")).toHaveValue("Roadmap");
  await page.locator("[data-note-menu-open]").click();
  await expect(page.locator("[data-note-menu-public]")).toBeVisible();
  await expect(page.locator("[data-note-menu-public]")).toHaveAttribute("href", `/notes/${FIRST_ID}`);
  await page.locator("[data-note-to-task]").click();
  await expect(page.locator('[data-note-notice="main"]')).toHaveText("Task created from this note");
  const origin = new URL(page.url()).origin;
  const task = calls.find((call) => call.path === "/api/tasks");
  expect(task.method).toBe("POST");
  expect(task.body).toEqual({
    title: "Roadmap",
    details: `Hello\n\n---\nConverted from the note "Roadmap" (published at ${origin}/notes/${FIRST_ID})`,
    department: "general",
    destination: "department",
    assigneeKind: "unassigned",
  });
});

test("a note turns into an issue in the repository the user names", async ({ page }) => {
  const calls = [];
  await openNotes(page, { search: `?note=${SECOND_ID}`, calls });
  await expect(page.locator("[data-note-title]")).toHaveValue("Retro");
  await page.locator("[data-note-menu-open]").click();
  await expect(page.locator("[data-note-menu-public]")).toBeHidden();
  await page.locator("[data-note-to-issue]").click();
  const dialog = page.locator("[data-note-issue-dialog]");
  await expect(dialog).toBeVisible();
  await page.locator("[data-note-issue-repo]").fill("project");
  await page.locator("[data-note-issue-confirm]").click();
  await expect(page.locator('[data-note-notice="issue"]')).toHaveText("Choose a repository as owner/name.");
  await expect(dialog).toBeVisible();
  await page.locator("[data-note-issue-repo]").fill("alice/project");
  await page.locator("[data-note-issue-repo]").press("Enter");
  await expect(dialog).toBeHidden();
  await expect(page.locator('[data-note-notice="main"]')).toHaveText("Issue filed from this note");
  const issue = calls.find((call) => call.path === "/api/repo/alice/project/issues");
  expect(issue.body).toMatchObject({ owner: "alice", repo: "project", number: 0, titleIfNew: "Retro" });
  expect(issue.body.event).toMatchObject({
    type: "open",
    title: "Retro",
    body: 'Sprint lessons learned\n\n---\nConverted from the note "Retro"',
  });
});

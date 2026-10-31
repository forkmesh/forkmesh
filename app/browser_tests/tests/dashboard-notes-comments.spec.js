const { test, expect } = require("@playwright/test");
const path = require("node:path");

const NOTES_PAGE_PATH = path.resolve(__dirname, "..", "..", "public", "dashboard", "notes", "index.html");
const NOTE_ID = "c".repeat(32);
const THREAD_ID = "d".repeat(32);
const MARKDOWN = "Hello world, this is the plan for the launch.";

async function openNotes(page, { search = "", comments = [], calls = [], others = [] } = {}) {
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alice", email: "alice@example.test", emailVerified: true,
      sessionToken: "test-session", status: "active",
    }));
    localStorage.setItem("forkmesh.notes.activity", "closed");
    window.ForkMeshDashboard = {
      _registrations: [["notes", ({ builtinPage }) => builtinPage("notes")]],
      registerPage(pageId, initializer) { this._registrations.push([pageId, initializer]); },
    };
    window.__sockets = [];
    window.__sent = [];
    class FakeSocket {
      constructor(url) {
        this.url = String(url); this.readyState = 0;
        window.__sockets.push(this);
        setTimeout(() => { this.readyState = 1; this.onopen?.(); }, 0);
      }
      send(data) { window.__sent.push(JSON.parse(data)); }
      close() { this.readyState = 3; }
      emit(frame) { this.onmessage?.({ data: JSON.stringify(frame) }); }
    }
    FakeSocket.CONNECTING = 0; FakeSocket.OPEN = 1; FakeSocket.CLOSING = 2; FakeSocket.CLOSED = 3;
    window.WebSocket = FakeSocket;
  });
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.tailwind = {};" }));
  await page.route("https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.lucide = { createIcons() {} };" }));
  await page.route(/\/dashboard\/notes(\?.*)?$/, (route) => route.fulfill({
    path: NOTES_PAGE_PATH, contentType: "text/html; charset=utf-8",
  }));
  const note = {
    id: NOTE_ID, title: "Launch", markdown: MARKDOWN, version: 1, role: "owner",
    visibility: "private", updatedAt: Date.now(), shares: [{ type: "user", name: "bob", role: "editor" }],
    links: [], webSocketUrl: `/api/notes/${NOTE_ID}/ws`,
  };
  const payload = () => ({ ok: true, comments, people: ["alice", "bob"], canResolve: true });
  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const body = request.postDataJSON();
    calls.push({ method: request.method(), path: url.pathname, body });
    let answer = { ok: true };
    if (url.pathname === "/api/notes") {
      const { markdown, ...summary } = note;
      answer = { ok: true, notes: [summary, ...others] };
    } else if (url.pathname === `/api/notes/${NOTE_ID}`) {
      answer = { ok: true, note };
    } else if (others.some((other) => url.pathname === `/api/notes/${other.id}`)) {
      answer = { ok: true, note: { ...others.find((other) => url.pathname.endsWith(other.id)), markdown: "Other" } };
    } else if (url.pathname === `/api/notes/${NOTE_ID}/comments`) {
      if (request.method() === "POST") {
        const comment = {
          id: String(comments.length + 1).padStart(32, "e"), parentId: body.parentId || "",
          author: "alice", body: body.body, quote: body.quote || "", prefix: body.prefix || "",
          suffix: body.suffix || "", start: body.start || 0, end: body.end || 0,
          mentions: /@bob\b/.test(body.body) ? ["bob"] : [], createdAt: Date.now(),
          updatedAt: Date.now(), editedAt: 0, resolvedAt: 0, resolvedBy: "",
        };
        comments.push(comment);
        answer = { ...payload(), comment };
      } else answer = payload();
    } else if (url.pathname.startsWith(`/api/notes/${NOTE_ID}/comments/`)) {
      const comment = comments.find((item) => url.pathname.endsWith(item.id));
      if (body && "resolved" in body) {
        comment.resolvedAt = body.resolved ? Date.now() : 0;
        comment.resolvedBy = body.resolved ? "alice" : "";
      }
      answer = { ...payload(), comment };
    } else if (url.pathname.endsWith("/activity")) {
      answer = { ok: true, counts: {}, events: [] };
    }
    return route.fulfill({ status: 200, contentType: "application/json; charset=utf-8", body: JSON.stringify(answer) });
  });
  await page.goto(`/dashboard/notes${search}`);
  await expect(page.locator("[data-note-title]")).toHaveValue("Launch");
}

test("commenting on a selection sends the quote with its context and mentions", async ({ page }) => {
  const comments = [];
  const calls = [];
  await openNotes(page, { comments, calls });
  const area = page.locator("[data-note-markdown]");
  await area.evaluate((node) => { node.focus(); node.setSelectionRange(6, 11); });
  await area.dispatchEvent("select");
  await page.locator("[data-note-comment-selection]").click();
  const composer = page.locator("[data-note-comment-new]");
  await expect(composer.locator("blockquote")).toHaveText("world");
  const input = composer.locator("[data-note-comment-input]");
  await input.pressSequentially("Check this @b");
  const options = page.locator("[data-note-mention-option]");
  await expect(options).toHaveCount(1);
  await expect(options.first()).toContainText("bob");
  await input.press("Enter");
  await expect(input).toHaveValue("Check this @bob ");
  await input.press("Enter");

  await expect.poll(() => calls.filter((call) => call.method === "POST" && call.path.endsWith("/comments")).length).toBe(1);
  const sent = calls.find((call) => call.method === "POST" && call.path.endsWith("/comments")).body;
  expect(sent).toMatchObject({ body: "Check this @bob", quote: "world", prefix: "Hello ", start: 6, end: 11 });
  expect(sent.suffix).toBe(MARKDOWN.slice(11, 43));
  const thread = page.locator("[data-note-thread]");
  await expect(thread).toHaveCount(1);
  await expect(thread.locator(".notes-mention")).toHaveText("@bob");
  await expect(page.locator("[data-note-comments-count]")).toHaveText("1");
  await expect(page.locator("[data-note-preview] [data-note-thread-mark]")).toHaveText("world");

  await thread.locator("[data-note-thread-resolve]").click();
  await expect(page.locator("[data-note-comment-resolved-label]")).toHaveText("Resolved (1)");
  await expect(page.locator("[data-note-comments-count]")).toHaveText("Comments");
});

test("a comment link opens its thread and moved text is still found", async ({ page }) => {
  const comments = [{
    id: THREAD_ID, parentId: "", author: "bob", body: "Which launch?", quote: "launch",
    prefix: "plan for the ", suffix: ".", start: 2, end: 8, mentions: [], createdAt: Date.now(),
    updatedAt: Date.now(), editedAt: 0, resolvedAt: 0, resolvedBy: "",
  }, {
    id: "f".repeat(32), parentId: "", author: "bob", body: "Gone", quote: "removed words",
    prefix: "", suffix: "", start: 0, end: 13, mentions: [], createdAt: Date.now(),
    updatedAt: Date.now(), editedAt: 0, resolvedAt: 0, resolvedBy: "",
  }];
  await openNotes(page, { search: `?note=${NOTE_ID}&comment=${THREAD_ID}`, comments });
  await expect(page.locator("[data-note-comments]")).toBeVisible();
  await expect(page.locator(`[data-note-thread="${THREAD_ID}"]`)).toHaveAttribute("data-focus", "true");
  await expect(page.locator(`[data-note-thread="${"f".repeat(32)}"] .notes-thread-quote`)).toHaveAttribute("data-detached", "true");
  await page.locator(`[data-note-thread="${THREAD_ID}"] [data-note-thread-jump]`).click();
  const range = await page.locator("[data-note-markdown]").evaluate((node) => [node.selectionStart, node.selectionEnd]);
  expect(range).toEqual([MARKDOWN.indexOf("launch"), MARKDOWN.indexOf("launch") + 6]);
});

test("presence shows who is viewing or editing and drops people who leave", async ({ page }) => {
  await openNotes(page);
  await expect.poll(() => page.evaluate(() => window.__sockets.length)).toBe(1);
  await expect(page.locator("[data-note-live]")).toHaveText("Live");
  await page.evaluate(() => window.__sockets[0].emit({ type: "presence", clientId: "bob-1", name: "bob", state: "viewing" }));
  await expect(page.locator("[data-note-live]")).toHaveText("Live · bob is viewing");
  await expect(page.locator("[data-note-presence] .notes-face")).toHaveAttribute("data-state", "viewing");
  await page.evaluate(() => window.__sockets[0].emit({ type: "presence", clientId: "bob-1", name: "bob", state: "editing" }));
  await expect(page.locator("[data-note-live]")).toHaveText("bob is editing…");
  await expect(page.locator("[data-note-presence] .notes-face")).toHaveAttribute("data-state", "editing");
  await page.locator("[data-note-presence]").click();
  await expect(page.locator("[data-note-people-list]")).toContainText("You");
  await expect(page.locator("[data-note-people-list]")).toContainText("Editing");
  await page.evaluate(() => window.__sockets[0].emit({ type: "leave", clientId: "bob-1", name: "bob" }));
  await expect(page.locator("[data-note-live]")).toHaveText("Live");
  await expect(page.locator("[data-note-presence]")).toBeHidden();

  await page.locator("[data-note-markdown]").press("End");
  await page.locator("[data-note-markdown]").pressSequentially(" More");
  await expect.poll(() => page.evaluate(() => window.__sent.some((frame) => frame.type === "presence" && frame.state === "editing"))).toBe(true);
});

test("the list marks notes changed by others and shows who they are shared with", async ({ page }) => {
  const other = {
    id: "9".repeat(32), title: "Budget", version: 3, role: "editor", visibility: "public",
    updatedAt: Date.now() - 60_000, lastEditedBy: "bob", unread: true, unreadComments: 2, openComments: 2,
    shares: [{ type: "user", name: "bob", role: "editor" }, { type: "user", name: "carol", role: "viewer" },
      { type: "organization", name: "acme", role: "viewer" }, { type: "user", name: "dan", role: "viewer" }],
  };
  await openNotes(page, { others: [other] });
  const row = page.locator(`[data-note-id="${other.id}"]`);
  await expect(row).toHaveAttribute("data-unread", "true");
  await expect(row.locator(".notes-row-dot")).toHaveAttribute("title", /2 new comments/);
  await expect(row.locator(".notes-row-vis")).toHaveText("Public");
  await expect(row.locator(".notes-row-people .notes-avatar")).toHaveCount(4);
  await expect(row.locator(".notes-row-more")).toHaveText("+1");
  await expect(row.locator(".notes-row-when")).toHaveText(/^Edited Today .+ · by bob$/);
  await expect(page.locator(`[data-note-id="${NOTE_ID}"] .notes-row-vis`)).toHaveText("Shared");
  await row.click();
  await expect(row).toHaveAttribute("data-unread", "false");
});

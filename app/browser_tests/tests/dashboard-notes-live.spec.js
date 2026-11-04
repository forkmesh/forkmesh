const { test, expect } = require("@playwright/test");
const path = require("node:path");

const NOTES_PAGE_PATH = path.resolve(__dirname, "..", "..", "public", "dashboard", "notes", "index.html");
const NOTE_ID = "c".repeat(32);
const MARKDOWN = "# Plan\n\nFirst paragraph here.\n\n- one\n- two\n\nLast words.";

async function openNotes(page, { calls = [], messages = [], mode = "live", markdown = MARKDOWN, activity = [], versions = null, note: noteFields = {} } = {}) {
  await page.addInitScript((initialMode) => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alice", email: "alice@example.test", emailVerified: true,
      sessionToken: "test-session", status: "active",
    }));
    localStorage.setItem("forkmesh.notes.activity", "closed");
    localStorage.setItem("forkmesh.notes.mode", initialMode);
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
  }, mode);
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.tailwind = {};" }));
  await page.route("https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.lucide = { createIcons() {} };" }));
  await page.route(/\/dashboard\/notes(\?.*)?$/, (route) => route.fulfill({
    path: NOTES_PAGE_PATH, contentType: "text/html; charset=utf-8",
  }));
  const note = {
    id: NOTE_ID, title: "Launch", markdown, version: 1, role: "owner",
    visibility: "private", updatedAt: Date.now(), shares: [{ type: "user", name: "bob", role: "editor" }],
    links: [], webSocketUrl: `/api/notes/${NOTE_ID}/ws`,
  };
  Object.assign(note, noteFields);
  const history = versions || [{ version: 1, createdAt: Date.now(), author: "alice" }];
  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const body = request.postDataJSON();
    calls.push({ method: request.method(), path: url.pathname, body });
    let answer = { ok: true };
    if (url.pathname === "/api/notes") {
      const { markdown: _markdown, ...summary } = note;
      answer = { ok: true, notes: [summary] };
    } else if (url.pathname === `/api/notes/${NOTE_ID}`) {
      if (request.method() === "PATCH") Object.assign(note, { markdown: body.markdown ?? note.markdown, title: body.title ?? note.title });
      answer = { ok: true, note };
    } else if (url.pathname === `/api/notes/${NOTE_ID}/comments`) {
      answer = { ok: true, comments: [], people: ["alice", "bob"], canResolve: true };
      if (request.method() === "POST") {
        answer.comment = { id: "e".repeat(32), parentId: "", author: "alice", body: body.body, quote: body.quote, prefix: body.prefix,
          suffix: body.suffix, start: body.start, end: body.end, mentions: [], createdAt: Date.now(), updatedAt: Date.now(),
          editedAt: 0, resolvedAt: 0, resolvedBy: "" };
        answer.comments = [answer.comment];
      }
    } else if (url.pathname === `/api/notes/${NOTE_ID}/chat`) {
      if (request.method() === "POST") {
        messages.push({ id: String(messages.length + 1).padStart(32, "a"), author: "alice", body: body.body, createdAt: Date.now() });
      }
      answer = { ok: true, messages, canModerate: true };
      if (request.method() === "POST") answer.message = messages[messages.length - 1];
    } else if (url.pathname === `/api/notes/${NOTE_ID}/versions`) {
      const number = Number(url.searchParams.get("version"));
      answer = url.searchParams.has("version")
        ? { ok: true, version: { ...history.find((item) => item.version === number), title: `Draft ${number}`, markdown: `Old **v${number}**`, emoji: "" } }
        : { ok: true, versions: history };
    } else if (url.pathname.endsWith("/activity")) {
      answer = { ok: true, counts: { versions: note.version }, events: activity };
    }
    return route.fulfill({ status: 200, contentType: "application/json; charset=utf-8", body: JSON.stringify(answer) });
  });
  await page.goto("/dashboard/notes");
  await expect(page.locator("[data-note-title]")).toHaveValue("Launch");
}

test("live mode edits the formatted note one block at a time", async ({ page }) => {
  const calls = [];
  await openNotes(page, { calls });
  await expect(page.locator("[data-note-work]")).toHaveAttribute("data-mode", "live");
  await expect(page.locator("[data-note-write-pane]")).toBeHidden();
  const blocks = page.locator("[data-note-preview] .notes-live-block");
  await expect(blocks).toHaveCount(4);
  await expect(blocks.nth(1)).toHaveText("First paragraph here.");

  await blocks.nth(1).click();
  const input = page.locator("[data-note-live-input]");
  await expect(input).toHaveValue("First paragraph here.");
  await input.press("End");
  await input.pressSequentially(" More **bold**");
  await expect(page.locator("[data-note-markdown]")).toHaveValue(
    "# Plan\n\nFirst paragraph here. More **bold**\n\n- one\n- two\n\nLast words.");
  await expect(blocks).toHaveCount(4);
  await input.press("ArrowDown");
  await expect(input).toHaveValue("- one\n- two");
  await input.press("Escape");
  await expect(input).toHaveCount(0);
  await expect(blocks.nth(1).locator("strong")).toHaveText("bold");

  await expect.poll(() => calls.filter((call) => call.method === "PATCH").length).toBeGreaterThan(0);
  expect(calls.filter((call) => call.method === "PATCH").pop().body.markdown).toContain("More **bold**");

  const box = await page.locator("[data-note-preview]").boundingBox();
  await page.mouse.click(box.x + 20, box.y + box.height - 4);
  await expect(input).toHaveValue("");
  await input.pressSequentially("New ending.");
  await expect(page.locator("[data-note-markdown]")).toHaveValue(/Last words\.\n\nNew ending\.$/);
  await input.press("Escape");
  await expect(blocks).toHaveCount(5);

  const heading = page.locator("[data-note-preview-title]");
  await heading.click();
  await page.keyboard.press("End");
  await page.keyboard.type(" day");
  await expect(page.locator("[data-note-title]")).toHaveValue("Launch day");
});

test("selected text in a live block can be commented on", async ({ page }) => {
  const calls = [];
  await openNotes(page, { calls });
  await page.locator("[data-note-preview] .notes-live-block").nth(1).click();
  const input = page.locator("[data-note-live-input]");
  await input.evaluate((node) => { node.focus(); node.setSelectionRange(6, 15); });
  await input.dispatchEvent("select");
  await page.keyboard.press("Control+Alt+KeyM");
  await expect(page.locator("[data-note-comment-new] blockquote")).toHaveText("paragraph");
  await page.locator("[data-note-comment-new] [data-note-comment-input]").fill("Shorter?");
  await page.locator("[data-note-comment-new] [data-note-comment-input]").press("Enter");
  await expect.poll(() => calls.filter((call) => call.method === "POST" && call.path.endsWith("/comments")).length).toBe(1);
  const sent = calls.find((call) => call.method === "POST" && call.path.endsWith("/comments")).body;
  expect(sent).toMatchObject({ quote: "paragraph", start: MARKDOWN.indexOf("paragraph"), end: MARKDOWN.indexOf("paragraph") + 9 });
});

test("following a collaborator keeps their place in view until you type", async ({ page }) => {
  const long = Array.from({ length: 80 }, (_, index) => `Line ${index + 1} of the plan.`).join("\n\n");
  await openNotes(page, { mode: "split", markdown: long });
  await expect.poll(() => page.evaluate(() => window.__sockets.length)).toBe(1);
  await page.evaluate(() => window.__sockets[0].emit({ type: "presence", clientId: "web-bob", name: "bob", state: "editing", line: 0, column: 0 }));
  await page.locator("[data-note-presence]").click();
  await page.locator("[data-note-follow-person=\"bob\"]").click();
  await expect(page.locator("[data-note-follow]")).toBeVisible();
  await expect(page.locator("[data-note-follow-label]")).toHaveText("Following bob");

  await page.evaluate(() => window.__sockets[0].emit({ type: "cursor", clientId: "web-bob", name: "bob", state: "editing", line: 141, column: 3 }));
  await expect(page.locator("[data-note-follow-tag]")).toBeVisible();
  await expect(page.locator("[data-note-follow-tag]")).toHaveText("bob");
  await expect(page.locator("[data-note-preview] [data-follow-name=\"bob\"]")).toContainText("Line 71 of the plan.");
  expect(await page.evaluate(() => document.querySelector("[data-note-markdown]").scrollTop
    + document.querySelector("[data-note-write-pane]").scrollTop)).toBeGreaterThan(1000);

  const area = page.locator("[data-note-markdown]");
  await area.evaluate((node) => { node.focus(); node.setSelectionRange(0, 0); });
  await area.pressSequentially("x");
  await expect(page.locator("[data-note-follow]")).toBeHidden();
  await expect.poll(() => page.evaluate(() => window.__sent.filter((frame) => frame.type === "cursor").pop()))
    .toMatchObject({ type: "cursor", line: 1, column: 1 });
});

test("chat in the side panel sends messages and counts unread ones", async ({ page }) => {
  const calls = [];
  const messages = [{ id: "b".repeat(32), author: "bob", body: "Morning!", createdAt: Date.now() }];
  await openNotes(page, { calls, messages, mode: "preview" });
  await expect(page.locator("[data-note-chat-badge]")).toHaveText("1");
  await page.locator("[data-note-chat-toggle]").click();
  await expect(page.locator("[data-note-panel=\"chat\"]")).toBeVisible();
  await expect(page.locator("[data-note-panel=\"comments\"]")).toBeHidden();
  await expect(page.locator("[data-note-chat-badge]")).toBeHidden();
  await expect(page.locator("[data-note-chat-list] .notes-chat-msg")).toHaveText([/bob.*Morning!/s]);

  const input = page.locator("[data-note-chat-input]");
  await input.fill("Hi bob <b>there</b>");
  await input.press("Enter");
  await expect.poll(() => calls.filter((call) => call.method === "POST" && call.path.endsWith("/chat")).length).toBe(1);
  await expect(input).toHaveValue("");
  await expect(page.locator("[data-note-chat-list] .notes-chat-msg").last()).toContainText("Hi bob <b>there</b>");

  await page.locator("[data-note-panel-tab=\"comments\"]").click();
  messages.push({ id: "c".repeat(32), author: "bob", body: "Seen it", createdAt: Date.now() + 1000 });
  await page.evaluate(() => window.__sockets[0].emit({ type: "note-chat", actor: "bob" }));
  await expect(page.locator("[data-note-chat-unread]")).toHaveText("1");
  await page.locator("[data-note-panel-tab=\"chat\"]").click();
  await expect(page.locator("[data-note-chat-unread]")).toBeHidden();
  await expect(page.locator("[data-note-chat-list] .notes-chat-msg")).toHaveCount(3);
});

test("the right column shows activity and history live beside the chat", async ({ page }) => {
  const now = Date.now();
  const activity = [{ kind: "create", actor: "alice", at: now - 60_000, version: 1 }];
  const versions = [{ version: 1, createdAt: now - 60_000, author: "alice" }];
  await openNotes(page, { mode: "preview", activity, versions });
  await page.locator("[data-note-activity-toggle]").click();
  const column = page.locator("[data-note-comments]");
  await expect(column).toBeVisible();
  await expect(page.locator("[data-note-panel-tab=\"activity\"]")).toHaveAttribute("aria-selected", "true");
  await expect(page.locator("[data-note-activity-list] .notes-act")).toHaveText([/You.*created the note/s]);

  activity.unshift({ kind: "share", actor: "alice", at: now, detail: { name: "bob", role: "editor" } });
  await page.evaluate(() => window.__sockets[0].emit({ type: "note-activity" }));
  await expect(page.locator("[data-note-activity-list] .notes-act").first()).toContainText("shared it with bob");

  await page.locator("[data-note-panel-tab=\"history\"]").click();
  await expect(page.locator("[data-note-panel=\"activity\"]")).toBeHidden();
  await expect(page.locator("[data-note-versions] .notes-version")).toHaveCount(1);
  await expect(page.locator("[data-note-versions] .notes-version")).toContainText(["Version 1"]);

  versions.unshift({ version: 2, createdAt: now, author: "bob" });
  await page.evaluate(() => window.__sockets[0].emit({ type: "note-updated", version: 1, actor: "bob" }));
  await expect(page.locator("[data-note-versions] .notes-version")).toHaveCount(2);
  await expect(page.locator("[data-note-versions] .notes-version").first()).toContainText("bob");

  await page.locator("[data-note-version-show=\"2\"]").click();
  await expect(page.locator("[data-note-version-view]")).toBeVisible();
  await expect(page.locator("[data-note-version-view] h1")).toHaveText("Draft 2");
  await expect(page.locator("[data-note-version-view] strong")).toHaveText("v2");
  await expect(page.locator("[data-note-version-view] [data-note-restore=\"2\"]")).toBeVisible();
  await page.locator("[data-note-version-back]").click();
  await expect(page.locator("[data-note-versions]")).toBeVisible();

  await page.locator("[data-note-panel-tab=\"chat\"]").click();
  await expect(page.locator("[data-note-chat-form]")).toBeVisible();
  await page.locator("[data-note-comments-close]").click();
  await expect(column).toBeHidden();
  await page.locator("[data-note-menu-open]").click();
  await page.locator("[data-note-versions-open]").click();
  await expect(column).toBeVisible();
  await expect(page.locator("[data-note-panel=\"history\"]")).toBeVisible();
});

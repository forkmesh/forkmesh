const { test, expect } = require("@playwright/test");
const path = require("node:path");

const NOTES_PAGE_PATH = path.resolve(__dirname, "..", "..", "public", "dashboard", "notes", "index.html");
const NOTE_ID = "c".repeat(32);
const MARKDOWN = "# Plan\n\nFirst paragraph here.\n\n- one\n- two\n\nLast words.";

async function openNotes(page, { calls = [], messages = [], reactions = [], mode = "", markdown = MARKDOWN, activity = [], versions = null, note: noteFields = {} } = {}) {
  await page.addInitScript((initialMode) => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alice", email: "alice@example.test", emailVerified: true,
      sessionToken: "test-session", status: "active",
    }));
    localStorage.setItem("forkmesh.notes.activity", "closed");
    if (initialMode) localStorage.setItem("forkmesh.notes.layout", initialMode);
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
      answer = { ok: true, comments: [], reactions, people: ["alice", "bob"], canResolve: true };
      if (request.method() === "POST") {
        answer.comment = { id: "e".repeat(32), parentId: "", author: "alice", body: body.body, quote: body.quote, prefix: body.prefix,
          suffix: body.suffix, start: body.start, end: body.end, mentions: [], createdAt: Date.now(), updatedAt: Date.now(),
          editedAt: 0, resolvedAt: 0, resolvedBy: "" };
        answer.comments = [answer.comment];
      }
    } else if (url.pathname.startsWith(`/api/notes/${NOTE_ID}/reactions`)) {
      if (request.method() === "POST") {
        reactions.push({ id: String(reactions.length + 1).padStart(32, "f"), author: "alice", createdAt: Date.now(), ...body });
      } else {
        reactions.splice(reactions.findIndex((item) => url.pathname.endsWith(item.id)), 1);
      }
      answer = { ok: true, comments: [], reactions, people: ["alice", "bob"], canResolve: true };
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

test("a note opens in Live when no layout was picked before", async ({ page }) => {
  await openNotes(page);
  await expect(page.locator("[data-note-work]")).toHaveAttribute("data-mode", "live");
  await page.locator("[data-note-mode=\"split\"]").click();
  expect(await page.evaluate(() => localStorage.getItem("forkmesh.notes.layout"))).toBe("split");
});

test("reacting to a passage shows the reaction beside it for everyone", async ({ page }) => {
  const calls = [], reactions = [];
  await openNotes(page, { calls, reactions });
  const blocks = page.locator("[data-note-preview] .notes-live-block");
  await blocks.nth(1).click();
  const input = page.locator("[data-note-live-input]");
  await input.evaluate((node) => { node.focus(); node.setSelectionRange(6, 15); });
  const box = await input.boundingBox();
  await input.dispatchEvent("mouseup", { clientX: box.x + 40, clientY: box.y + 8 });
  await page.locator("[data-note-react-float]").click();
  await page.locator("[data-note-reaction-pick=\"👍\"]").click();
  await expect.poll(() => calls.filter((call) => call.method === "POST" && call.path.endsWith("/reactions")).length).toBe(1);
  expect(calls.find((call) => call.path.endsWith("/reactions")).body).toMatchObject({
    emoji: "👍", quote: "paragraph", start: MARKDOWN.indexOf("paragraph"), end: MARKDOWN.indexOf("paragraph") + 9 });
  await expect(input).toHaveCount(0);
  const chip = blocks.nth(1).locator("[data-note-reaction-toggle]");
  await expect(chip).toHaveText("👍1");
  await expect(chip).toHaveAttribute("aria-pressed", "true");
  await expect(blocks.nth(1).locator(".notes-reaction-quote")).toHaveText("“paragraph”");

  reactions.push({ id: "b".repeat(32), author: "bob", emoji: "👍", quote: "paragraph", prefix: "First ", suffix: " here.",
    start: MARKDOWN.indexOf("paragraph"), end: MARKDOWN.indexOf("paragraph") + 9, createdAt: Date.now() });
  reactions.push({ id: "d".repeat(32), author: "bob", emoji: "🎉", quote: "", prefix: "", suffix: "", start: 0, end: 0, createdAt: Date.now() });
  await page.evaluate(() => window.__sockets[0].emit({ type: "note-comments", actor: "bob" }));
  await expect(chip).toHaveText("👍2");
  await expect(chip).toHaveAttribute("title", "alice, bob reacted with 👍");
  await expect(page.locator("[data-note-reactions-note] [data-note-reaction-toggle]")).toHaveText("🎉1");
  await expect(page.locator("[data-note-reactions-note] [data-note-reaction-toggle]")).toHaveAttribute("aria-pressed", "false");

  await chip.click();
  await expect.poll(() => calls.filter((call) => call.method === "DELETE").map((call) => call.path))
    .toEqual([`/api/notes/${NOTE_ID}/reactions/${"1".padStart(32, "f")}`]);
  await expect(chip).toHaveText("👍1");
  await expect(chip).toHaveAttribute("aria-pressed", "false");
  await expect(input).toHaveCount(0);
});

test("live mode marks the block each collaborator is editing", async ({ page }) => {
  await openNotes(page);
  await expect.poll(() => page.evaluate(() => window.__sockets.length)).toBe(1);
  const blocks = page.locator("[data-note-preview] .notes-live-block");
  await page.evaluate(() => window.__sockets[0].emit({ type: "cursor", clientId: "web-bob", name: "bob", state: "editing", line: 3, column: 2 }));
  await expect(blocks.nth(1)).toHaveAttribute("data-peer-editing", "bob");
  await page.evaluate(() => window.__sockets[0].emit({ type: "cursor", clientId: "web-bob", name: "bob", state: "editing", line: 6, column: 0 }));
  await expect(blocks.nth(2)).toHaveAttribute("data-peer-editing", "bob");
  await expect(blocks.nth(1)).not.toHaveAttribute("data-peer-editing", /./);
  await page.evaluate(() => window.__sockets[0].emit({ type: "leave", clientId: "web-bob", name: "bob" }));
  await expect(page.locator("[data-note-preview] [data-peer-editing]")).toHaveCount(0);
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

const SECTIONS = ["Alpha", "Beta", "Gamma", "Delta", "Epsilon"];
const SECTIONED = SECTIONS.map((name) => `## ${name}\n\n${Array.from({ length: 14 }, (_, index) => `${name} paragraph ${index + 1}.`).join("\n\n")}`).join("\n\n");

function headingOffsetInPane(page, name) {
  return page.evaluate((text) => {
    const pane = document.querySelector("[data-note-preview-pane]");
    const heading = [...pane.querySelectorAll("[data-note-preview] h2")].find((node) => node.textContent === text);
    return heading.getBoundingClientRect().top - pane.getBoundingClientRect().top;
  }, name);
}

test("split mode keeps the preview level with the Markdown and the outline jumps to a section", async ({ page }) => {
  await openNotes(page, { mode: "split", markdown: SECTIONED });
  const outline = page.locator("[data-note-outline]");
  await expect(outline).toBeHidden();
  await page.locator("[data-note-outline-toggle]").click();
  await expect(outline).toBeVisible();
  await expect(outline.locator("[data-note-outline-entry]")).toHaveText(SECTIONS);

  await outline.locator("[data-note-outline-entry]", { hasText: "Delta" }).click();
  await expect.poll(() => headingOffsetInPane(page, "Delta")).toBeLessThan(40);
  expect(await headingOffsetInPane(page, "Delta")).toBeGreaterThan(-40);
  await expect(outline.locator("[aria-current=\"true\"]")).toHaveText("Delta");
  const area = page.locator("[data-note-markdown]");
  expect(await area.evaluate((node) => node.value.slice(node.selectionStart).startsWith("## Delta"))).toBe(true);

  await area.evaluate((node) => { node.dispatchEvent(new Event("mouseenter")); node.scrollTop = 0; });
  await expect.poll(() => page.evaluate(() => document.querySelector("[data-note-preview-pane]").scrollTop)).toBeLessThan(2);
  await expect(outline.locator("[aria-current=\"true\"]")).toHaveCount(0);

  const pane = page.locator("[data-note-preview-pane]");
  await pane.hover();
  await page.mouse.wheel(0, 900);
  await expect.poll(() => area.evaluate((node) => node.scrollTop)).toBeGreaterThan(300);
  await expect(page.locator("[data-note-outline-entry][aria-current=\"true\"]")).toHaveCount(1);

  await page.reload();
  await expect(page.locator("[data-note-outline]")).toBeVisible();
});

async function pasteImage(page, width, height, text = "") {
  await page.evaluate(async ({ width, height, text }) => {
    const canvas = document.createElement("canvas");
    canvas.width = width; canvas.height = height;
    const context = canvas.getContext("2d");
    const gradient = context.createLinearGradient(0, 0, width, height);
    gradient.addColorStop(0, "#2f81f7"); gradient.addColorStop(1, "#bf4b8a");
    context.fillStyle = gradient; context.fillRect(0, 0, width, height);
    const blob = await new Promise((resolve) => canvas.toBlob(resolve, "image/png"));
    const data = new DataTransfer();
    data.items.add(new File([blob], "image.png", { type: "image/png" }));
    if (text) data.setData("text/plain", text);
    const area = document.querySelector("[data-note-markdown]");
    area.focus();
    area.setSelectionRange(area.value.length, area.value.length);
    area.dispatchEvent(new ClipboardEvent("paste", { clipboardData: data, bubbles: true, cancelable: true }));
  }, { width, height, text });
}

test("pasting an image from the clipboard puts it in the note, made smaller when it is wide", async ({ page }) => {
  const calls = [];
  await openNotes(page, { calls, mode: "split" });
  const area = page.locator("[data-note-markdown]");

  await pasteImage(page, 40, 20);
  await expect(area).toHaveValue(/Last words\.\n!\[image\]\(data:image\/png;base64,[A-Za-z0-9+/=]+\)$/);
  await expect(page.locator("[data-note-preview] img")).toHaveCount(1);

  await pasteImage(page, 3200, 1200);
  await expect(page.locator("[data-note-preview] img")).toHaveCount(2);
  const wide = page.locator("[data-note-preview] img").nth(1);
  await expect.poll(() => wide.evaluate((node) => node.naturalWidth)).toBe(1600);
  expect((await area.inputValue()).length).toBeLessThan(512 * 1024);
  await expect.poll(() => calls.filter((call) => call.method === "PATCH").length).toBeGreaterThan(0);
  expect(calls.filter((call) => call.method === "PATCH").pop().body.markdown).toContain("![image](data:image/");

  const before = await area.inputValue();
  await pasteImage(page, 40, 20, "copied cell");
  await expect(page.locator("[data-note-preview] img")).toHaveCount(2);
  expect(await area.inputValue()).toBe(before);
});

const IMAGE = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAHgAAABQCAIAAABd+SbeAAAAqElEQVR4nO3QAQkAIADAMJOY07CGsYXCHTzA2Zhr60Lj+cEngQbdCjToVqBBtwINuhVo0K1Ag24FGnQr0KBbgQbdCjToVqBBtwINuhVo0K1Ag24FGnQr0KBbgQbdCjToVqBBtwINuhVo0K1Ag24FGnQr0KBbgQbdCjToVqBBtwINuhVo0K1Ag24FGnQr0KBbgQbdCjToVqBBtwINuhVo0K1Ag24FGnSrA35MNiuKw9RgAAAAAElFTkSuQmCC";
const PAGED = `# Plan\n\n![logo](${IMAGE})\n\n${Array.from({ length: 40 }, (_, index) => `Paragraph ${index + 1} of the plan, long enough to wrap across the line in the page.`).join("\n\n")}`;

test("pages shows the note on paper and image options place an image behind the text", async ({ page }) => {
  const calls = [];
  await openNotes(page, { calls, mode: "pages", markdown: PAGED });
  const frame = page.frameLocator("[data-note-pages-frame]");
  await expect(frame.locator(".np-page").first()).toBeVisible();
  const pages = await frame.locator(".np-page").count();
  expect(pages).toBeGreaterThan(1);
  await expect(page.locator("[data-note-pages-count]")).toHaveText(`${pages} pages`);
  await expect(frame.locator(".np-page").first().locator(".np-number")).toHaveText(`1 / ${pages}`);
  await expect(frame.locator(".np-page").nth(1).locator(".np-win")).toContainText("Paragraph");

  await frame.locator(".np-win img[data-note-image-line]").first().click();
  await expect(page.locator("[data-note-image-toolbar]")).toBeVisible();
  await page.locator("[data-note-image-wrap-open]").click();
  await page.locator("[data-note-image-wrap-menu] [data-note-image-wrap=\"wrap\"]").click();
  await expect(page.locator("[data-note-markdown]")).toHaveValue(/\n<!-- image: wrap=wrap -->\n!\[logo\]/);

  await page.locator("[data-note-image-options-open]").click();
  const panel = page.locator("[data-note-image-panel]");
  await expect(panel).toBeVisible();
  await expect(panel.locator("[data-note-image-field=\"x\"]")).toBeDisabled();
  await panel.locator("[data-note-image-wrap=\"behind\"]").click();
  await expect(panel.locator("[data-note-image-field=\"x\"]")).toBeEnabled();
  await panel.locator("[data-note-image-action=\"fill\"]").click();
  await expect(page.locator("[data-note-markdown]")).toHaveValue(/<!-- image: wrap=behind; width=210; height=297 -->\n!\[logo\]/);
  await panel.locator("[data-note-image-field=\"pages\"]").selectOption("all");
  await panel.locator("summary", { hasText: "Recolor" }).click();
  await panel.locator("[data-note-image-recolor=\"grayscale\"]").click();
  await expect(page.locator("[data-note-markdown]")).toHaveValue(/<!-- image: wrap=behind; width=210; height=297; pages=all; recolor=grayscale -->/);
  await expect.poll(() => frame.locator(".np-layer-behind img").count()).toBe(pages);
  await expect(frame.locator(".np-page").first().locator(".np-layer-behind img")).toHaveCSS("filter", "grayscale(1)");
  await expect(frame.locator(".np-win img")).toHaveCount(0);
  await page.screenshot({ path: process.env.FORKMESH_SHOT ? `${process.env.FORKMESH_SHOT}/pages.png` : undefined });

  await panel.locator("summary", { hasText: "Alt text" }).click();
  await panel.locator("[data-note-image-field=\"alt\"]").fill("Company logo");
  await panel.locator("[data-note-image-field=\"alt\"]").blur();
  await expect(page.locator("[data-note-markdown]")).toHaveValue(/\n!\[Company logo\]\(data:image\/png/);
  await expect.poll(() => calls.filter((call) => call.method === "PATCH").length).toBeGreaterThan(0);

  await page.locator("[data-note-pages-export]").click();
  await page.locator("[data-note-pdf-confirm]").click();
  const print = page.frameLocator("iframe[data-note-print-frame]");
  await expect(print.locator(".np-page")).toHaveCount(pages);
  await expect(print.locator(".np-page").first().locator(".np-layer-behind img")).toHaveCount(1);
});

test("an image in Live can be resized and wrapped beside the text", async ({ page }) => {
  await openNotes(page, { markdown: `Intro.\n\n![logo](${IMAGE})\n\nText that wraps around the picture.` });
  const image = page.locator("[data-note-preview] img[data-note-image-line]");
  await image.click();
  await expect(page.locator("[data-note-live-input]")).toHaveCount(0);
  await expect(page.locator("[data-note-image-select]")).toBeVisible();
  const handle = page.locator("[data-note-image-select] span");
  const box = await handle.boundingBox();
  await page.mouse.move(box.x + 5, box.y + 5);
  await page.mouse.down();
  await page.mouse.move(box.x + 120, box.y + 5, { steps: 4 });
  await page.mouse.up();
  await expect(page.locator("[data-note-markdown]")).toHaveValue(/<!-- image: width=\d+(\.\d)? -->\n!\[logo\]/);
  await page.locator("[data-note-image-wrap-open]").click();
  await page.locator("[data-note-image-wrap-menu] [data-note-image-wrap=\"wrap\"]").click();
  await expect(image).toHaveCSS("float", "left");
  await page.keyboard.press("Escape");
  await expect(page.locator("[data-note-image-toolbar]")).toBeHidden();
});

test("a background added in Pages is selected, moves, is picked through the text, deletes and undoes", async ({ page }) => {
  await openNotes(page, { mode: "pages", markdown: PAGED });
  const frame = page.frameLocator("[data-note-pages-frame]");
  const area = page.locator("[data-note-markdown]");
  await expect(frame.locator(".np-page").first()).toBeVisible();
  const chooser = page.waitForEvent("filechooser");
  await page.locator("[data-note-insert-image=\"background\"]").click();
  await (await chooser).setFiles({ name: "paper.png", mimeType: "image/png", buffer: Buffer.from(IMAGE.split(",")[1], "base64") });
  await expect(area).toHaveValue(/<!-- image: wrap=behind; width=210; height=297; pages=all -->\n!\[background\]\(data:image\/png/);
  await expect(page.locator("[data-note-image-toolbar]")).toBeVisible();
  await expect(frame.locator(".np-select")).toHaveCount(1);
  const inserted = await area.inputValue();

  await page.keyboard.press("Escape");
  await expect(page.locator("[data-note-image-toolbar]")).toBeHidden();
  const text = frame.locator(".np-page").first().locator(".np-win p", { hasText: "Paragraph 3 of" });
  await text.click();
  await expect(page.locator("[data-note-image-toolbar]")).toBeVisible();

  const box = await text.boundingBox();
  const x = box.x + 20, y = box.y + box.height / 2;
  await page.mouse.move(x, y);
  await page.mouse.down();
  await page.mouse.move(x + 40, y + 30, { steps: 6 });
  await page.mouse.up();
  await expect(area).toHaveValue(/<!-- image: wrap=behind; width=210; height=297; x=\d+(\.\d)?; y=\d+(\.\d)?; pages=all -->/);
  const movedLeft = await frame.locator(".np-page").first().locator(".np-layer-behind img").evaluate((node) => node.style.left);
  expect(movedLeft).not.toBe("0mm");

  await page.keyboard.press("Control+z");
  await expect(area).toHaveValue(inserted);
  await expect(page.locator("[data-note-image-toolbar]")).toBeVisible();

  await page.keyboard.press("Delete");
  await expect(area).not.toHaveValue(/!\[background\]/);
  await expect(page.locator("[data-note-image-toolbar]")).toBeHidden();
  await page.keyboard.press("Control+z");
  await expect(area).toHaveValue(inserted);
  await expect(page.locator("[data-note-image-toolbar]")).toBeVisible();
  await page.keyboard.press("Control+Shift+z");
  await expect(area).not.toHaveValue(/!\[background\]/);

  await page.keyboard.press("Control+z");
  await expect(page.locator("[data-note-image-toolbar]")).toBeVisible();
  await page.locator("[data-note-image-delete]").click();
  await expect(area).not.toHaveValue(/!\[background\]/);
});

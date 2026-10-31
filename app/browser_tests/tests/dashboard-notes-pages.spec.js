const { test, expect } = require("@playwright/test");
const path = require("node:path");

const NOTES_PAGE_PATH = path.resolve(__dirname, "..", "..", "public", "dashboard", "notes", "index.html");
const LAUNCH = "1".repeat(32);
const BUDGET = "2".repeat(32);
const RETRO = "3".repeat(32);
const LOCKED = "4".repeat(32);
const PAGE_ID = "a1".repeat(16);

function fixtures() {
  const notes = {
    [LAUNCH]: { id: LAUNCH, title: "Launch", emoji: "", markdown: "Launch plan body", version: 1, role: "owner", visibility: "private", updatedAt: Date.now(), shares: [], links: [] },
    [BUDGET]: { id: BUDGET, title: "Budget", emoji: "💰", markdown: "Budget numbers body", version: 4, role: "editor", visibility: "private", updatedAt: Date.now(), shares: [], links: [], unread: true, lastEditedBy: "bob" },
    [RETRO]: { id: RETRO, title: "Retro", emoji: "🧪", markdown: "Retro lessons body", version: 2, role: "owner", visibility: "private", updatedAt: Date.now(), shares: [], links: [] },
  };
  const page = {
    id: PAGE_ID, title: "Handbook", emoji: "📚", description: "The **team** handbook", accent: "purple",
    layout: "outline", showEmoji: true, role: "owner", version: 2, createdAt: Date.now(), updatedAt: Date.now(), shares: [],
    outline: [
      { id: "sec-start1", kind: "section", depth: 0, label: "Start here", noteId: "", url: "", collapsed: false },
      { id: "ent-launch", kind: "note", depth: 1, label: "", noteId: LAUNCH, url: "", collapsed: false },
      { id: "ent-budget", kind: "note", depth: 2, label: "", noteId: BUDGET, url: "", collapsed: false },
      { id: "ent-locked", kind: "note", depth: 1, label: "", noteId: LOCKED, url: "", collapsed: false },
      { id: "ent-docs01", kind: "link", depth: 0, label: "Docs", noteId: "", url: "https://example.com/docs", collapsed: false },
    ],
  };
  return { notes, page };
}

function pageSummary(page) {
  const { outline, ...rest } = page;
  return { ...rest, entries: outline.length };
}

function pageNotes(page, notes, content) {
  const map = {};
  for (const entry of page.outline) {
    const note = notes[entry.noteId];
    if (entry.kind !== "note" || !note) continue;
    map[note.id] = { id: note.id, title: note.title, emoji: note.emoji, updatedAt: note.updatedAt, version: note.version,
      role: note.role, lastEditedBy: note.lastEditedBy || "", unread: Boolean(note.unread) };
    if (content && note.id !== BUDGET) map[note.id].markdown = note.markdown;
  }
  return map;
}

async function openNotes(page, { search = "", calls = [] } = {}) {
  const { notes, page: notePage } = fixtures();
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
    window.__copied = [];
    Object.defineProperty(navigator, "clipboard", {
      configurable: true,
      value: { writeText: async (text) => { window.__copied.push(text); } },
    });
    class FakeSocket {
      constructor() { this.readyState = 0; setTimeout(() => { this.readyState = 1; this.onopen?.(); }, 0); }
      send() {}
      close() { this.readyState = 3; }
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
  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const method = request.method();
    const body = request.postDataJSON();
    calls.push({ method, path: url.pathname, search: url.search, body });
    let status = 200;
    let answer = { ok: true };
    const noteMatch = url.pathname.match(/^\/api\/notes\/([0-9a-f]{32})$/);
    if (url.pathname === "/api/notes") {
      answer = { ok: true, notes: Object.values(notes).map(({ markdown, ...summary }) => summary) };
    } else if (url.pathname === "/api/notes/pages" && method === "GET") {
      answer = { ok: true, pages: [pageSummary(notePage)] };
    } else if (url.pathname === "/api/notes/pages" && method === "POST") {
      const created = { ...notePage, id: "b2".repeat(16), title: "Untitled page", emoji: "", description: "", accent: "blue", version: 1, outline: [] };
      status = 201; answer = { ok: true, page: created };
    } else if (url.pathname === `/api/notes/pages/${PAGE_ID}` && method === "GET") {
      answer = { ok: true, page: notePage, notes: pageNotes(notePage, notes, url.searchParams.get("content") === "1") };
    } else if (url.pathname === `/api/notes/pages/${PAGE_ID}` && method === "PATCH") {
      if (body.baseVersion !== notePage.version) {
        status = 409; answer = { error: "version_conflict", page: notePage };
      } else {
        const { baseVersion, ...fields } = body;
        Object.assign(notePage, fields, { version: notePage.version + 1 });
        answer = { ok: true, page: notePage };
      }
    } else if (url.pathname === `/api/notes/pages/${PAGE_ID}/shares`) {
      notePage.shares = [...notePage.shares.filter((share) => share.name !== body.name), { type: body.type, name: body.name, role: body.role }];
      answer = { ok: true, shares: notePage.shares, sharedNotes: body.includeNotes ? [LAUNCH] : [] };
    } else if (url.pathname === "/api/chat/direct-messages/users") {
      answer = { ok: true, users: [{ name: "carol" }] };
    } else if (noteMatch && notes[noteMatch[1]]) {
      const note = notes[noteMatch[1]];
      if (method === "PATCH") {
        Object.assign(note, { title: body.title, markdown: body.markdown, version: note.version + 1 });
        if ("emoji" in body) note.emoji = body.emoji;
      }
      answer = { ok: true, note };
    } else if (url.pathname.endsWith("/activity")) {
      answer = { ok: true, counts: {}, events: [] };
    } else if (url.pathname.endsWith("/comments")) {
      answer = { ok: true, comments: [], people: [], canResolve: true };
    }
    return route.fulfill({ status, contentType: "application/json; charset=utf-8", body: JSON.stringify(answer) });
  });
  await page.goto(`/dashboard/notes${search}`);
  return { notes, notePage };
}

const patches = (calls, pathname) => calls.filter((call) => call.method === "PATCH" && call.path === pathname);

test("the emoji picker saves the note's emoji, refuses text and removes it", async ({ page }) => {
  const calls = [];
  await openNotes(page, { calls });
  await expect(page.locator("[data-note-title]")).toHaveValue("Launch");
  await page.locator("[data-note-emoji-open]").click();
  await expect(page.locator("[data-note-emoji-choice]")).toHaveCount(48);
  await page.locator('[data-note-emoji-choice="🚀"]').click();
  await expect.poll(() => patches(calls, `/api/notes/${LAUNCH}`).length).toBe(1);
  expect(patches(calls, `/api/notes/${LAUNCH}`)[0].body).toMatchObject({ emoji: "🚀", baseVersion: 1, title: "Launch" });
  await expect(page.locator("[data-note-emoji-open]")).toHaveText("🚀");
  await expect(page.locator(`[data-note-id="${LAUNCH}"] .notes-row-emoji`)).toHaveText("🚀");
  await expect(page.locator("[data-note-crumb-emoji]")).toHaveText("🚀");
  await expect(page.locator("[data-note-preview-emoji]")).toHaveText("🚀");

  await page.locator("[data-note-emoji-open]").click();
  await page.locator("[data-note-emoji-input]").fill("ab");
  await page.locator("[data-note-emoji-form] button[type=submit]").click();
  await expect(page.locator('[data-note-notice="emoji"]')).toContainText("isn't an emoji");
  const keycap = `1${String.fromCodePoint(0xFE0F, 0x20E3)}`;
  await page.locator("[data-note-emoji-input]").fill(keycap);
  await page.locator("[data-note-emoji-input]").press("Enter");
  await expect.poll(() => patches(calls, `/api/notes/${LAUNCH}`).length).toBe(2);
  expect(patches(calls, `/api/notes/${LAUNCH}`)[1].body).toMatchObject({ emoji: keycap, baseVersion: 2 });

  await page.locator("[data-note-emoji-open]").click();
  await page.locator("[data-note-emoji-remove]").click();
  await expect.poll(() => patches(calls, `/api/notes/${LAUNCH}`).length).toBe(3);
  expect(patches(calls, `/api/notes/${LAUNCH}`)[2].body).toMatchObject({ emoji: "", baseVersion: 3 });
  await expect(page.locator(`[data-note-id="${LAUNCH}"] .notes-row-emoji`)).toHaveCount(0);
});

test("a page shows its outline with sections, indented notes, a locked entry and a link", async ({ page }) => {
  const calls = [];
  await openNotes(page, { calls });
  const row = page.locator(`[data-note-page-id="${PAGE_ID}"]`);
  await expect(row).toContainText("Handbook");
  await expect(row.locator(".notes-page-count")).toHaveText("5");
  await expect(page.locator("[data-note-page-new]")).not.toHaveClass(/notes-new/);
  await row.click();

  await expect(page.locator("[data-note-page-view]")).toBeVisible();
  await expect(page.locator("[data-note-editor]")).toBeHidden();
  await expect(page.locator("[data-note-page-title]")).toHaveText("Handbook");
  await expect(page.locator("[data-note-page-emoji-view]")).toHaveText("📚");
  await expect(page.locator("[data-note-page-description] strong")).toHaveText("team");
  await expect(page).toHaveURL(new RegExp(`\\?page=${PAGE_ID}&note=${LAUNCH}$`));
  expect(calls.some((call) => call.path === `/api/notes/pages/${PAGE_ID}` && call.search === "?content=1")).toBe(true);

  const outline = page.locator("[data-note-page-outline]");
  await expect(outline.locator('[data-kind="section"]')).toHaveText("Start here");
  const launch = outline.locator('[data-note-page-entry="ent-launch"]');
  const budget = outline.locator('[data-note-page-entry="ent-budget"]');
  await expect(launch).toHaveAttribute("style", /--depth:1/);
  await expect(launch).toHaveAttribute("aria-current", "true");
  await expect(budget).toHaveAttribute("style", /--depth:2/);
  await expect(budget.locator(".notes-page-entry-emoji")).toHaveText("💰");
  await expect(budget.locator(".notes-row-dot")).toHaveCount(1);
  await expect(outline.locator('[data-kind="locked"]')).toContainText("You don't have access to this note");
  const link = outline.locator('[data-kind="link"]');
  await expect(link).toHaveText("Docs");
  await expect(link).toHaveAttribute("href", "https://example.com/docs");
  await expect(link).toHaveAttribute("target", "_blank");
  await expect(page.locator("[data-note-page-notes]")).toContainText("Launch plan body");

  await budget.click();
  await expect(page.locator("[data-note-page-notes]")).toContainText("Budget numbers body");
  expect(calls.some((call) => call.method === "GET" && call.path === `/api/notes/${BUDGET}`)).toBe(true);
  await expect(budget.locator(".notes-row-dot")).toHaveCount(0);
  await expect(page).toHaveURL(new RegExp(`note=${BUDGET}`));

  await outline.locator('[data-note-page-section="sec-start1"]').click();
  await expect(launch).toHaveCount(0);
  await expect(outline.locator('[data-kind="locked"]')).toHaveCount(0);
  await expect(link).toBeVisible();
  await outline.locator('[data-note-page-section="sec-start1"]').click();
  await expect(launch).toBeVisible();

  await page.locator(`[data-note-page-open-editor="${BUDGET}"]`).click();
  await expect(page.locator("[data-note-editor]")).toBeVisible();
  await expect(page.locator("[data-note-page-view]")).toBeHidden();
  await expect(page.locator("[data-note-title]")).toHaveValue("Budget");
  await expect(page).toHaveURL(new RegExp(`\\?note=${BUDGET}$`));
});

test("switching the layout to continuous shows every note and the outline jumps between them", async ({ page }) => {
  const calls = [];
  await openNotes(page, { calls, search: `?page=${PAGE_ID}` });
  await expect(page.locator("[data-note-page-title]")).toHaveText("Handbook");
  await expect(page.locator("[data-note-page-notes] .notes-page-note")).toHaveCount(1);
  await page.locator("[data-note-page-customize-open]").click();
  await page.locator('[data-note-page-layout-choice="continuous"]').click();
  await page.locator("[data-note-page-save]").click();
  await expect.poll(() => patches(calls, `/api/notes/pages/${PAGE_ID}`).length).toBe(1);
  expect(patches(calls, `/api/notes/pages/${PAGE_ID}`)[0].body).toMatchObject({ layout: "continuous", baseVersion: 2 });
  await expect(page.locator("[data-note-page-customize]")).toBeHidden();

  const notes = page.locator("[data-note-page-notes]");
  await expect(notes.locator(".notes-page-note")).toHaveCount(2);
  await expect(notes.locator('[data-note-page-anchor="sec-start1"]')).toHaveText("Start here");
  await expect(notes.locator("#note-page-ent-launch")).toContainText("Launch plan body");
  await expect(notes.locator("#note-page-ent-budget")).toContainText("Budget numbers body");
  await expect(notes.locator(".notes-page-locked")).toContainText("You don't have access to this note");
  await page.setViewportSize({ width: 1280, height: 420 });
  await page.locator('[data-note-page-outline] [data-note-page-entry="ent-budget"]').click();
  await expect(notes.locator("#note-page-ent-budget")).toBeInViewport();
  await expect(page).toHaveURL(new RegExp(`note=${BUDGET}`));
});

test("customizing the outline adds, indents and moves a note and saves the outline", async ({ page }) => {
  const calls = [];
  await openNotes(page, { calls, search: `?page=${PAGE_ID}` });
  await expect(page.locator("[data-note-page-title]")).toHaveText("Handbook");
  await page.locator("[data-note-page-customize-open]").click();
  const drawer = page.locator("[data-note-page-customize]");
  await expect(drawer.locator("[data-note-page-edit]")).toHaveCount(5);
  await drawer.locator('[data-note-page-field="title"]').fill("Team handbook");
  await drawer.locator('[data-note-page-accent-choice="teal"]').click();
  await expect(drawer.locator('[data-note-page-accent-choice="teal"]')).toHaveAttribute("aria-checked", "true");

  await drawer.locator('[data-note-page-add="note"]').click();
  await drawer.locator("[data-note-page-add-search]").fill("ret");
  await expect(drawer.locator("[data-note-page-add-note]")).toHaveCount(1);
  await drawer.locator(`[data-note-page-add-note="${RETRO}"]`).click();
  const rows = drawer.locator("[data-note-page-edit]");
  await expect(rows).toHaveCount(6);
  const added = rows.last();
  await expect(added.locator("[data-note-page-entry-label]")).toHaveAttribute("placeholder", "Retro");
  await added.locator('[data-note-page-edit-action="indent"]').click();
  await expect(rows.last()).toHaveAttribute("style", /--depth:1/);
  await rows.last().locator('[data-note-page-edit-action="up"]').click();
  await expect(rows.nth(4).locator("[data-note-page-entry-label]")).toHaveAttribute("placeholder", "Retro");
  await rows.nth(2).locator('[data-note-page-edit-action="outdent"]').click();
  await expect(rows.nth(2)).toHaveAttribute("style", /--depth:1/);

  await drawer.locator('[data-note-page-add="section"]').click();
  await page.keyboard.type("Later");
  await expect(rows.last().locator("[data-note-page-entry-label]")).toHaveValue("Later");

  await drawer.locator("[data-note-page-save]").click();
  await expect.poll(() => patches(calls, `/api/notes/pages/${PAGE_ID}`).length).toBe(1);
  const sent = patches(calls, `/api/notes/pages/${PAGE_ID}`)[0].body;
  expect(sent).toMatchObject({ baseVersion: 2, title: "Team handbook", accent: "teal", layout: "outline", showEmoji: true });
  expect(sent.outline.map((entry) => [entry.kind, entry.depth, entry.noteId || entry.label])).toEqual([
    ["section", 0, "Start here"],
    ["note", 1, LAUNCH],
    ["note", 1, BUDGET],
    ["note", 1, LOCKED],
    ["note", 1, RETRO],
    ["link", 0, "Docs"],
    ["section", 0, "Later"],
  ]);
  const ids = sent.outline.map((entry) => entry.id);
  expect(new Set(ids).size).toBe(ids.length);
  for (const id of ids) expect(id).toMatch(/^[A-Za-z0-9_-]{8,32}$/);
  expect(sent.outline[5]).toMatchObject({ url: "https://example.com/docs", noteId: "" });
  await expect(page.locator("[data-note-page-title]")).toHaveText("Team handbook");
  await expect(page.locator('[data-note-page-outline] [data-note-page-entry]')).toHaveCount(3);
});

test("a stale page version offers to load the latest", async ({ page }) => {
  const calls = [];
  const { notePage } = await openNotes(page, { calls, search: `?page=${PAGE_ID}` });
  await expect(page.locator("[data-note-page-title]")).toHaveText("Handbook");
  await page.locator("[data-note-page-customize-open]").click();
  notePage.version = 5;
  await page.locator('[data-note-page-field="title"]').fill("Mine");
  await page.locator("[data-note-page-save]").click();
  await expect(page.locator("[data-note-page-conflict]")).toBeVisible();
  await page.locator("[data-note-page-reload]").click();
  await expect(page.locator("[data-note-page-conflict]")).toBeHidden();
  await expect(page.locator('[data-note-page-field="title"]')).toHaveValue("Handbook");
  await page.locator("[data-note-page-save]").click();
  await expect.poll(() => patches(calls, `/api/notes/pages/${PAGE_ID}`).length).toBe(2);
  expect(patches(calls, `/api/notes/pages/${PAGE_ID}`)[1].body.baseVersion).toBe(5);
});

test("sharing a page sends includeNotes and copies the page link", async ({ page }) => {
  const calls = [];
  await openNotes(page, { calls, search: `?page=${PAGE_ID}` });
  await expect(page.locator("[data-note-page-title]")).toHaveText("Handbook");
  await page.locator("[data-note-page-share-open]").click();
  const panel = page.locator("[data-note-page-share-panel]");
  await expect(panel).toBeVisible();
  await expect(panel.locator("[data-note-page-share-include]")).toBeChecked();
  const input = panel.locator("[data-note-page-share-name]");
  await input.pressSequentially("car");
  await expect(panel.locator("[data-note-page-share-option]").first()).toContainText("carol");
  await input.press("Enter");
  await expect(input).toHaveValue("carol");
  await panel.locator("button[type=submit]").click();
  await expect.poll(() => calls.filter((call) => call.method === "POST" && call.path.endsWith("/shares")).length).toBe(1);
  expect(calls.find((call) => call.method === "POST" && call.path === `/api/notes/pages/${PAGE_ID}/shares`).body)
    .toEqual({ type: "user", name: "carol", role: "editor", includeNotes: true });
  await expect(panel.locator("[data-note-page-shares]")).toContainText("carol");
  await expect(page.locator('[data-note-notice="page-share"]')).toContainText("Shared the page and your notes on it with carol");

  await panel.locator("[data-note-page-share-include]").uncheck();
  await panel.locator("[data-note-page-share-role]").selectOption("viewer");
  await input.fill("dave");
  await input.press("Enter");
  await expect.poll(() => calls.filter((call) => call.method === "POST" && call.path.endsWith("/shares")).length).toBe(2);
  expect(calls.filter((call) => call.method === "POST" && call.path.endsWith("/shares"))[1].body)
    .toEqual({ type: "user", name: "dave", role: "viewer", includeNotes: false });

  await panel.locator("[data-note-page-copy-link]").click();
  await expect.poll(() => page.evaluate(() => window.__copied)).toEqual([`http://127.0.0.1:4179/dashboard/notes?page=${PAGE_ID}`]);
});

test("a page link with a note opens the page with that note picked", async ({ page }) => {
  const calls = [];
  await openNotes(page, { calls, search: `?page=${PAGE_ID}&note=${BUDGET}` });
  await expect(page.locator("[data-note-page-title]")).toHaveText("Handbook");
  await expect(page.locator('[data-note-page-entry="ent-budget"]')).toHaveAttribute("aria-current", "true");
  await expect(page.locator("[data-note-page-notes]")).toContainText("Budget numbers body");
  await expect(page).toHaveURL(new RegExp(`\\?page=${PAGE_ID}&note=${BUDGET}$`));
  expect(calls.some((call) => call.method === "GET" && call.path === `/api/notes/${LAUNCH}`)).toBe(false);
  await expect(page.locator(`[data-note-page-id="${PAGE_ID}"]`)).toHaveAttribute("aria-selected", "true");
});

test("on a phone the outline is a list you open above the content", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 800 });
  await openNotes(page, { search: `?page=${PAGE_ID}` });
  await expect(page.locator("[data-note-page-title]")).toHaveText("Handbook");
  const outline = page.locator("[data-note-page-outline]");
  await expect(outline).toBeHidden();
  await page.locator("[data-note-page-outline-toggle]").click();
  await expect(outline).toBeVisible();
  await outline.locator('[data-note-page-entry="ent-budget"]').click();
  await expect(outline).toBeHidden();
  await expect(page.locator("[data-note-page-notes]")).toContainText("Budget numbers body");
  await page.locator("[data-note-page-back]").click();
  await expect(page.locator(`[data-note-page-id="${PAGE_ID}"]`)).toBeVisible();
});

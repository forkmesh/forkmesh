const { test, expect } = require("@playwright/test");


// The dashboard chat has always honoured inbound "edit"/"delete" frames from
// the desktop client and the full chat page; these cover the author-side
// controls that produce them.
function mockChatApis(page) {
  return page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    let body = { error: "not_found" };
    let status = 404;
    if (url.pathname === "/api/chat/room-key") {
      status = 200;
      body = {
        ok: true,
        room: "world-general",
        access: "public-world-general",
        passphrase: "playwright-dashboard-message-actions-passphrase",
      };
    } else if (url.pathname === "/api/accounts/users") {
      status = 200;
      body = { ok: true, users: [] };
    } else if (url.pathname === "/api/chat/activity") {
      status = 200;
      body = { ok: true, messageCount: 0, latestMessageTs: 0, userCount: 0 };
    }
    return route.fulfill({
      status,
      contentType: "application/json",
      body: JSON.stringify(body),
    });
  });
}


test("dashboard chat edits and deletes your own message", async ({ page }) => {
  const retainedFrames = [];
  await mockChatApis(page);
  await page.routeWebSocket(
    "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
    (socket) => {
      socket.onMessage((message) => {
        if (JSON.parse(String(message)).persist === true) {
          retainedFrames.push(String(message));
        }
      });
    },
  );

  await page.goto("/dashboard/chat/index.html");
  await expect(page.locator("[data-dashboard-chat-status]").first()).toContainText(
    "Connected",
  );

  await page.locator("#fullChatAction").selectOption("chat");
  await page.locator("#fullChatInput").fill("first draft of my message");
  await page.locator("#fullChatSend").click();
  const row = page.locator("#fullChatMessages .group").first();
  await expect(row).toContainText("first draft of my message");
  await expect.poll(() => retainedFrames.length).toBe(1);

  // Edit: the inline editor replaces the text, and the saved message carries
  // an "(edited)" marker so readers see it was rewritten.
  await row.getByRole("button", { name: "Edit message" }).click();
  await row.locator("textarea").fill("second draft of my message");
  await row.getByRole("button", { name: "Save edit" }).click();
  await expect(row).toContainText("second draft of my message");
  await expect(row.locator(".chat-edited")).toHaveText("(edited)");
  await expect(row.locator("textarea")).toHaveCount(0);
  // The edit rides the room as its own durable frame, like the desktop client.
  await expect.poll(() => retainedFrames.length).toBe(2);

  // Delete: confirmed inline (never a blocking window.confirm, which would
  // freeze the World's same-origin chat embed), then the row is gone.
  await row.getByRole("button", { name: "Delete message" }).click();
  await row.getByRole("button", { name: "Confirm delete" }).click();
  await expect(page.locator("#fullChatMessages")).not.toContainText(
    "second draft of my message",
  );
  await expect.poll(() => retainedFrames.length).toBe(3);
});


// A peer's message is authored elsewhere, so it needs a second browser context
// (its own localStorage, hence its own chat id) relaying through the same
// mocked room: only the author gets the controls.
test("dashboard chat offers no edit or delete on someone else's message", async ({
  browser,
}) => {
  const room = "**/api/repo/mainnode/forkmesh/rooms/world-general/ws";
  const sockets = [];
  const contexts = [await browser.newContext(), await browser.newContext()];
  const pages = [];
  for (const context of contexts) {
    const page = await context.newPage();
    await mockChatApis(page);
    await page.routeWebSocket(room, (socket) => {
      sockets.push(socket);
      socket.onMessage((message) => {
        for (const peer of sockets) {
          if (peer !== socket) peer.send(String(message));
        }
      });
    });
    await page.goto("/dashboard/chat/index.html");
    await expect(page.locator("[data-dashboard-chat-status]").first())
      .toContainText("Connected");
    pages.push(page);
  }
  const [author, reader] = pages;

  await author.locator("#fullChatAction").selectOption("chat");
  await author.locator("#fullChatInput").fill("hello from a peer");
  await author.locator("#fullChatSend").click();

  const authorRow = author.locator("#fullChatMessages .group", {
    hasText: "hello from a peer",
  });
  await expect(authorRow.getByRole("button", { name: "Edit message" }))
    .toHaveCount(1);

  const readerRow = reader.locator("#fullChatMessages .group", {
    hasText: "hello from a peer",
  });
  await expect(readerRow).toBeVisible();
  await expect(readerRow.getByRole("button", { name: "Edit message" }))
    .toHaveCount(0);
  await expect(readerRow.getByRole("button", { name: "Delete message" }))
    .toHaveCount(0);

  for (const context of contexts) await context.close();
});

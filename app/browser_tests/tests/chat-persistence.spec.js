const { test, expect } = require("@playwright/test");


test("ForkBot sends the model selected in the prompt area without storage", async ({
  page,
}) => {
  const selectedModel = "@cf/meta/llama-4-scout-17b-16e-instruct";
  let forkbotRequest = null;

  await page.addInitScript(() => {
    localStorage.setItem(
      "forkmesh.session",
      JSON.stringify({
        nodeName: "alice",
        email: "alice@example.test",
        kind: "user",
        sessionToken: "test-session",
      }),
    );
    const originalSetItem = Storage.prototype.setItem;
    Storage.prototype.setItem = function setItem(key, value) {
      if (key === "forkmesh.forkbot.model") {
        throw new DOMException("Storage disabled", "SecurityError");
      }
      return originalSetItem.call(this, key, value);
    };
  });

  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    let body = { error: "not_found" };
    let status = 404;
    if (url.pathname === "/api/chat/room-key") {
      status = 200;
      body = {
        ok: true,
        room: "world-general",
        access: "public-world-general",
        passphrase: "playwright-forkbot-model-passphrase",
      };
    } else if (url.pathname === "/api/accounts/users") {
      status = 200;
      body = { ok: true, users: [] };
    } else if (url.pathname === "/api/chat/activity") {
      status = 200;
      body = { ok: true, messageCount: 0, latestMessageTs: 0, userCount: 0 };
    } else if (url.pathname === "/api/forkbot/models") {
      status = 200;
      body = {
        ok: true,
        default: "@cf/meta/llama-3.3-70b-instruct-fp8-fast",
        models: [
          {
            id: "@cf/meta/llama-3.3-70b-instruct-fp8-fast",
            label: "Llama 3.3 70B (fast)",
            default: true,
          },
          { id: selectedModel, label: "Llama 4 Scout 17B", default: false },
        ],
      };
    } else if (url.pathname === "/api/forkbot/chat") {
      status = 200;
      forkbotRequest = route.request().postDataJSON();
      body = { ok: true, botMessage: "Llama handled that prompt." };
    }
    return route.fulfill({
      status,
      contentType: "application/json",
      body: JSON.stringify(body),
    });
  });
  await page.routeWebSocket(
    "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
    () => {},
  );

  await page.goto("/chat.html");
  await expect(page.locator("#chat-status")).toContainText("Connected");
  const modelPicker = page.locator("#chat-forkbot-model");
  await expect(modelPicker).toBeVisible();
  await modelPicker.selectOption(selectedModel);
  await page.locator("#chat-input").fill("forkbot explain this failure");
  await page.locator("#chat-send").click();

  await expect.poll(() => forkbotRequest?.model).toBe(selectedModel);
  expect(forkbotRequest.message).toBe("forkbot explain this failure");
});


test("self-authored retained chat survives a page refresh", async ({ page }) => {
  const passphrase = "playwright-public-world-general-passphrase";
  const retainedFrames = [];
  let connectionCount = 0;

  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    let body = { error: "not_found" };
    let status = 404;
    if (url.pathname === "/api/chat/room-key") {
      status = 200;
      body = {
        ok: true,
        room: "world-general",
        access: "public-world-general",
        passphrase,
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

  await page.routeWebSocket(
    "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
    (socket) => {
      connectionCount += 1;
      socket.onMessage((message) => {
        const frame = String(message);
        if (JSON.parse(frame).persist === true) retainedFrames.push(frame);
      });
      if (connectionCount > 1) {
        setTimeout(() => {
          retainedFrames.forEach((frame) => socket.send(frame));
        }, 50);
      }
    },
  );

  await page.goto("/chat.html");
  await expect(page.locator("#chat-status")).toContainText("Connected");
  await page.locator("#chat-input").fill("My message must survive refresh");
  await page.locator("#chat-send").click();
  await expect(page.locator("#chat-log")).toContainText(
    "My message must survive refresh",
  );
  await expect.poll(() => retainedFrames.length).toBe(1);

  await page.reload();

  await expect(page.locator("#chat-status")).toContainText("Connected");
  await expect(page.locator("#chat-log")).toContainText(
    "My message must survive refresh",
  );
});


test("a public chat message accepts and sends a thread reply", async ({ page }) => {
  const passphrase = "playwright-public-thread-passphrase";
  const retainedFrames = [];

  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    let body = { error: "not_found" };
    let status = 404;
    if (url.pathname === "/api/chat/room-key") {
      status = 200;
      body = {
        ok: true,
        room: "world-general",
        access: "public-world-general",
        passphrase,
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
  await page.routeWebSocket(
    "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
    (socket) => {
      socket.onMessage((message) => {
        const frame = String(message);
        if (JSON.parse(frame).persist === true) retainedFrames.push(frame);
      });
    },
  );

  await page.goto("/chat.html");
  await expect(page.locator("#chat-status")).toContainText("Connected");
  const rootText = "Thread root from the browser";
  const replyText = "Thread reply from the browser";
  await page.locator("#chat-input").fill(rootText);
  await page.locator("#chat-input").press("Enter");
  const root = page.locator(".chat-msg", { hasText: rootText });
  await expect(root).toBeVisible();
  await root.hover();
  await root.getByRole("button", { name: "Reply in thread" }).click();
  await expect(page.locator("#chat-thread-input")).toBeFocused();
  await page.locator("#chat-thread-input").fill(replyText);
  await page.locator("#chat-thread-send").click();
  await expect(page.locator("#chat-thread-replies")).toContainText(replyText);
  await expect.poll(() => retainedFrames.length).toBe(2);
  await page.locator("#chat-thread-close").click();
  await expect(page.locator(".chat-thread-view")).toBeHidden();
});


test("a confirmed delete removes and broadcasts a public chat message", async ({
  page,
}) => {
  const retainedFrames = [];
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    let body = { error: "not_found" };
    let status = 404;
    if (url.pathname === "/api/chat/room-key") {
      status = 200;
      body = {
        ok: true,
        room: "world-general",
        access: "public-world-general",
        passphrase: "playwright-public-delete-passphrase",
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
  await page.routeWebSocket(
    "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
    (socket) => {
      socket.onMessage((message) => {
        const frame = String(message);
        if (JSON.parse(frame).persist === true) retainedFrames.push(frame);
      });
    },
  );

  await page.goto("/chat.html");
  await expect(page.locator("#chat-status")).toContainText("Connected");
  const message = "Delete this public message";
  await page.locator("#chat-input").fill(message);
  await page.locator("#chat-input").press("Enter");
  const row = page.locator("#chat-log .chat-msg", { hasText: message });
  await expect(row).toBeVisible();
  await row.hover();
  await row.getByRole("button", { name: "Delete message" }).click();
  await expect(page.locator("#chat-delete-dialog")).toBeVisible();
  await page.locator("#chat-delete-confirm").click();
  await expect(page.locator("#chat-log")).not.toContainText(message);
  await expect.poll(() => retainedFrames.length).toBe(2);
});

const { test, expect } = require("@playwright/test");

const RETAINED_KEY_ADVERTISEMENTS = 1;


test("admin manages a private channel while only invited users join", async ({ page }) => {
  const channelId = "a".repeat(32);
  const channels = [{
    id: channelId,
    name: "release-team",
    updatedAt: Date.now(),
    keyVersion: 1,
    visibility: "private",
  }];
  const members = new Set(["alice"]);
  const retainedPrivateFrames = [];
  let privateConnections = 0;
  let keyVersion = 1;

  const session = (name, isAdmin = false) => ({
    kind: "user",
    nodeName: name,
    email: `${name}@example.test`,
    sessionToken: `${name}-token`,
    isAdmin,
  });
  await page.addInitScript((value) => {
    if (!localStorage.getItem("forkmesh.session")) {
      localStorage.setItem("forkmesh.session", JSON.stringify(value));
    }
  }, session("admin", true));

  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const authorization = request.headers().authorization || "";
    const actor = authorization.replace(/^Bearer\s+/i, "").replace(/-token$/, "");
    const isAdmin = actor === "admin";
    const channelPath = `/api/chat/channels/${channelId}`;
    let status = 404;
    let body = { error: "not_found" };

    if (url.pathname === "/api/chat/room-key") {
      status = 200;
      body = {
        ok: true,
        room: "world-general",
        access: "public-world-general",
        passphrase: "playwright-public-room-passphrase",
      };
    } else if (url.pathname === "/api/accounts/users") {
      status = 200;
      body = {
        ok: true,
        users: ["admin", "alice", "bob"].map((name) => ({
          name,
          createdAt: Date.now() - 60_000,
        })),
      };
    } else if (url.pathname === "/api/chat/activity") {
      status = 200;
      body = { ok: true, messageCount: 0, latestMessageTs: 0, userCount: 0 };
    } else if (url.pathname === "/api/chat/channels" && request.method() === "GET") {
      if (!actor) {
        status = 401;
        body = { error: "invalid_session" };
      } else {
        status = 200;
        body = {
          channels: channels
            .filter((channel) => (
              isAdmin || channel.visibility === "public" || members.has(actor)
            ))
            .map((channel) => ({ ...channel, canManage: isAdmin })),
        };
      }
    } else if (url.pathname === `${channelPath}/members`) {
      if (!isAdmin) {
        status = 403;
        body = { error: "admin_required" };
      } else if (request.method() === "GET") {
        status = 200;
        body = {
          members: [...members].map((username) => ({
            username,
            joinedAt: Date.now(),
          })),
        };
      } else if (request.method() === "POST") {
        const data = request.postDataJSON();
        members.add(data.username);
        status = 201;
        body = {
          ok: true,
          member: { username: data.username, joinedAt: Date.now() },
        };
      } else if (request.method() === "DELETE") {
        const data = request.postDataJSON();
        const removed = members.delete(data.username);
        if (removed) {
          keyVersion += 1;
          channels[0] = {
            ...channels[0],
            keyVersion,
            updatedAt: Date.now(),
          };
        }
        status = 200;
        body = { ok: true, removed, keyVersion };
      }
    } else if (
      url.pathname === `${channelPath}/room-access` &&
      (isAdmin || channels[0]?.visibility === "public" || members.has(actor))
    ) {
      status = 200;
      body = {
        channel: { ...channels[0], canManage: isAdmin },
        room: `chat-channel:${channelId}:v${keyVersion}`,
        passphrase: `playwright-private-release-team-passphrase-v${keyVersion}`,
        webSocketUrl: `${channelPath}/ws?ticket=${actor}-ticket`,
      };
    }

    await route.fulfill({
      status,
      contentType: "application/json",
      body: JSON.stringify(body),
    });
  });

  await page.routeWebSocket(
    "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
    () => {},
  );
  await page.routeWebSocket("**/api/chat/channels/*/ws?ticket=*", (socket) => {
    privateConnections += 1;
    socket.onMessage((message) => {
      const frame = String(message);
      if (JSON.parse(frame).persist === true) retainedPrivateFrames.push(frame);
    });
    if (privateConnections > 1) {
      const replay = [...retainedPrivateFrames];
      setTimeout(() => {
        replay.forEach((frame) => socket.send(frame));
      }, 50);
    }
  });
  const signInAs = async (value) => {
    await page.evaluate((stored) => {
      localStorage.setItem("forkmesh.session", JSON.stringify(stored));
    }, value);
    await page.reload();
  };

  await page.goto("/chat.html");
  await page.getByRole("button", {
    name: "Private channel #release-team",
  }).click();

  await expect(page.locator("#chat-channel-title")).toHaveText("#release-team");
  await expect(page.locator("#chat-channel-visibility-badge")).toHaveText("private");
  await expect(page.locator("#chat-status")).toHaveAttribute(
    "title",
    "Connected · private #release-team",
  );
  await expect(page.locator("#chat-channel-manage")).toBeVisible();
  await page.locator("#chat-channel-manage").click();
  await expect(page.locator("#chat-channel-members")).toContainText("@alice");
  await page.setViewportSize({ width: 390, height: 844 });
  await expect.poll(() => page.evaluate(
    () => document.documentElement.scrollWidth <= window.innerWidth,
  )).toBe(true);

  await page.locator("#chat-channel-settings-close").click();
  await expect(page.locator("#chat-log")).toContainText("No messages in #release-team yet.");

  const beforeAlice = retainedPrivateFrames.length;
  await signInAs(session("alice"));
  await expect(page.locator("#chat-channel-title")).toHaveText("#release-team");
  await expect(page.locator("#chat-status")).toHaveAttribute(
    "title",
    "Connected · private #release-team",
  );
  await expect.poll(() => retainedPrivateFrames.length).toBe(
    beforeAlice + RETAINED_KEY_ADVERTISEMENTS,
  );

  await signInAs(session("admin", true));
  await expect(page.locator("#chat-channel-title")).toHaveText("#release-team");
  await expect.poll(() => page.evaluate(() => {
    const profile = btoa("acct:admin").replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
    const prefix = `forkmesh.chat/e2ee/p/${profile}/trustV3/`;
    return Object.keys(localStorage)
      .filter((key) => key.startsWith(prefix))
      .flatMap((key) => Object.values(JSON.parse(localStorage.getItem(key)).records || {}))
      .some((record) => record.acct === "alice" && record.state === "approved");
  })).toBe(true);
  const beforeSend = retainedPrivateFrames.length;
  await page.locator("#chat-input").fill("Private release is ready");
  await page.locator("#chat-send").click();
  await expect(page.locator("#chat-log")).toContainText("Private release is ready");
  await expect.poll(() => retainedPrivateFrames.length).toBe(beforeSend + 1);
  expect(retainedPrivateFrames.join("\n")).not.toContain("Private release is ready");

  await signInAs(session("alice"));
  await expect(page.locator("#chat-channel-title")).toHaveText("#release-team");
  await expect(page.locator("#chat-log")).toContainText("Private release is ready");

  await page.evaluate((value) => {
    localStorage.setItem("forkmesh.session", JSON.stringify(value));
  }, session("admin", true));
  await page.reload();

  await expect(page.locator("#chat-channel-manage")).toBeVisible();
  await page.locator("#chat-channel-manage").click();
  await page.getByRole("button", { name: "Remove alice from channel" }).click();
  await expect(page.locator("#chat-channel-members")).not.toContainText("@alice");

  await page.evaluate((value) => {
    localStorage.setItem("forkmesh.session", JSON.stringify(value));
  }, session("alice"));
  await page.reload();

  await expect(page.locator("#chat-channel-title")).toHaveText("World");
  await expect(page.locator("#chat-rooms")).not.toContainText("release-team");

  await page.evaluate((value) => {
    localStorage.setItem("forkmesh.session", JSON.stringify(value));
  }, session("bob"));
  await page.reload();

  await expect(page.locator("#chat-channel-title")).toHaveText("World");
  await expect(page.locator("#chat-rooms")).not.toContainText("release-team");
});


test("registered users discover a relay-provided public channel", async ({ page }) => {
  const channelId = "b".repeat(32);
  const channel = {
    id: channelId,
    name: "announcements",
    updatedAt: Date.now(),
    keyVersion: 1,
    visibility: "public",
  };
  const session = (name, isAdmin = false) => ({
    kind: "user",
    nodeName: name,
    email: `${name}@example.test`,
    sessionToken: `${name}-token`,
    isAdmin,
  });

  await page.addInitScript((value) => {
    if (!localStorage.getItem("forkmesh.session")) {
      localStorage.setItem("forkmesh.session", JSON.stringify(value));
    }
  }, session("admin", true));

  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const authorization = request.headers().authorization || "";
    const actor = authorization.replace(/^Bearer\s+/i, "").replace(/-token$/, "");
    const isAdmin = actor === "admin";
    const channelPath = `/api/chat/channels/${channelId}`;
    let status = 404;
    let body = { error: "not_found" };

    if (url.pathname === "/api/chat/room-key") {
      status = 200;
      body = {
        ok: true,
        room: "world-general",
        access: "public-world-general",
        passphrase: "playwright-public-room-passphrase",
      };
    } else if (url.pathname === "/api/accounts/users") {
      status = 200;
      body = {
        ok: true,
        users: ["admin", "alice", "bob"].map((name) => ({
          name,
          createdAt: Date.now() - 60_000,
        })),
      };
    } else if (url.pathname === "/api/chat/activity") {
      status = 200;
      body = { ok: true, messageCount: 0, latestMessageTs: 0, userCount: 0 };
    } else if (url.pathname === "/api/chat/channels" && request.method() === "GET") {
      if (!actor) {
        status = 401;
        body = { error: "invalid_session" };
      } else {
        status = 200;
        body = { channels: [{ ...channel, canManage: isAdmin }] };
      }
    } else if (
      url.pathname === `${channelPath}/room-access` &&
      actor &&
      channel?.visibility === "public"
    ) {
      status = 200;
      body = {
        channel: { ...channel, canManage: isAdmin },
        room: `chat-channel:${channelId}:v1`,
        passphrase: "playwright-public-announcements-passphrase",
        webSocketUrl: `${channelPath}/ws?ticket=${actor}-ticket`,
      };
    }

    await route.fulfill({
      status,
      contentType: "application/json",
      body: JSON.stringify(body),
    });
  });

  await page.routeWebSocket(
    "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
    () => {},
  );
  await page.routeWebSocket("**/api/chat/channels/*/ws?ticket=*", () => {});

  await page.goto("/chat.html");
  await page.getByRole("button", {
    name: "Public channel #announcements",
  }).click();
  await expect(page.locator("#chat-channel-title")).toHaveText("#announcements");
  await expect(page.locator("#chat-channel-visibility-badge")).toHaveText("public");
  await expect(page.locator("#chat-status")).toHaveAttribute(
    "title",
    "Connected · public #announcements",
  );
  await expect(page.locator("#chat-channel-manage")).toBeVisible();

  await page.evaluate((value) => {
    localStorage.setItem("forkmesh.session", JSON.stringify(value));
  }, session("bob"));
  await page.reload();

  await expect(page.locator("#chat-channel-title")).toHaveText("#announcements");
  await expect(page.getByRole("button", { name: "Public channel #announcements" })).toBeVisible();
  await expect(page.locator("#chat-status")).toHaveAttribute(
    "title",
    "Connected · public #announcements",
  );
  await expect(page.locator("#chat-channel-manage")).toBeHidden();

  await page.setViewportSize({ width: 390, height: 844 });
  await expect.poll(() => page.evaluate(
    () => document.documentElement.scrollWidth <= window.innerWidth,
  )).toBe(true);

  await page.evaluate(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({ kind: "guest" }));
  });
  await page.reload();

  await expect(page.locator("#chat-channel-title")).toHaveText("World");
  await expect(page.locator("#chat-rooms")).not.toContainText("announcements");
});


test("a registered user creates a private channel and one-time webhook", async ({ page }) => {
  const channelId = "c".repeat(32);
  const webhookId = "d".repeat(32);
  const token = "e".repeat(64);
  const rotated = "b".repeat(64);
  const channels = [];
  const webhooks = [];
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      kind: "user",
      nodeName: "alice",
      email: "alice@example.test",
      sessionToken: "alice-token",
      isAdmin: false,
    }));
  });
  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    let status = 404;
    let body = { error: "not_found" };
    if (url.pathname === "/api/chat/room-key") {
      status = 200;
      body = { ok: true, room: "world-general", passphrase: "general-key" };
    } else if (url.pathname === "/api/chat/activity") {
      status = 200;
      body = { ok: true, messageCount: 0, latestMessageTs: 0, userCount: 0 };
    } else if (url.pathname === "/api/accounts/users") {
      status = 200;
      body = { ok: true, users: [{ name: "alice" }, { name: "bob" }] };
    } else if (url.pathname === "/api/chat/channels" && request.method() === "GET") {
      status = 200;
      body = { channels };
    } else if (url.pathname === "/api/chat/channels" && request.method() === "POST") {
      const data = request.postDataJSON();
      channels.push({
        id: channelId,
        name: data.name,
        visibility: data.visibility,
        keyVersion: 1,
        canManage: true,
      });
      status = 201;
      body = { ok: true, channel: channels[0], members: [{ username: "bob" }] };
    } else if (url.pathname === `/api/chat/channels/${channelId}/room-access`) {
      status = 200;
      body = {
        channel: channels[0],
        room: `chat-channel:${channelId}:v1`,
        passphrase: "private-key",
        webSocketUrl: `/api/chat/channels/${channelId}/ws?ticket=alice-ticket`,
      };
    } else if (url.pathname === `/api/chat/channels/${channelId}/members`) {
      status = 200;
      body = { members: [{ username: "bob", joinedAt: Date.now() }] };
    } else if (
      url.pathname === `/api/chat/channels/${channelId}/webhooks` &&
      request.method() === "GET"
    ) {
      status = 200;
      body = { webhooks };
    } else if (
      url.pathname === `/api/chat/channels/${channelId}/webhooks` &&
      request.method() === "POST"
    ) {
      const webhook = {
        id: webhookId,
        name: request.postDataJSON().name,
        createdAt: Date.now(),
        lastUsedAt: 0,
      };
      webhooks.push(webhook);
      status = 201;
      body = {
        ok: true,
        webhook,
        url: `https://forkmesh.test/api/chat/webhooks/${token}`,
      };
    } else if (
      url.pathname === `/api/chat/channels/${channelId}/webhooks/${webhookId}` &&
      request.method() === "POST"
    ) {
      status = 200;
      body = {
        ok: true,
        webhook: { ...webhooks[0], lastUsedAt: 0 },
        url: `https://forkmesh.test/api/chat/webhooks/${rotated}`,
      };
    }
    await route.fulfill({
      status,
      contentType: "application/json",
      body: JSON.stringify(body),
    });
  });
  await page.routeWebSocket(
    "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
    () => {},
  );
  await page.routeWebSocket("**/api/chat/channels/*/ws?ticket=*", () => {});

  await page.goto("/chat.html");
  await page.getByRole("button", { name: "Create channel" }).click();
  await page.locator("#chat-channel-name").fill("design-team");
  await page.locator("#chat-channel-initial-members").fill("bob");
  await page.locator("#chat-channel-create-form").getByRole("button", {
    name: "Create",
  }).click();
  await expect(page.locator("#chat-channel-title")).toHaveText("#design-team");
  await page.locator("#chat-channel-manage").click();
  await page.locator("#chat-channel-webhook-name").fill("Deploy bot");
  await page.getByRole("button", { name: "Add webhook" }).click();
  await expect(page.locator("#chat-channel-webhook-created")).toContainText(
    "shown only once",
  );
  await expect(page.locator("#chat-channel-webhooks")).toContainText("Deploy bot");
  await expect(page.locator("#chat-channel-webhooks")).toContainText(token);

  // The relay cannot show a token twice, so rotating is how a manager who
  // lost the URL gets a working one back.
  await page.getByRole("button", {
    name: "New URL for Deploy bot webhook",
  }).click();
  await expect(page.locator("#chat-channel-webhooks")).toContainText(rotated);
  await expect(page.locator("#chat-channel-webhooks")).not.toContainText(token);
});


test("a channel manager renames and deletes a channel from its settings", async ({ page }) => {
  const channelId = "f".repeat(32);
  const channels = [{
    id: channelId,
    name: "release-team",
    updatedAt: Date.now(),
    keyVersion: 1,
    visibility: "private",
    topic: "",
    canManage: true,
  }];
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      kind: "user",
      nodeName: "alice",
      email: "alice@example.test",
      sessionToken: "alice-token",
      isAdmin: false,
    }));
  });
  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const channelPath = `/api/chat/channels/${channelId}`;
    let status = 404;
    let body = { error: "not_found" };
    if (url.pathname === "/api/chat/room-key") {
      status = 200;
      body = { ok: true, room: "world-general", passphrase: "general-key" };
    } else if (url.pathname === "/api/chat/activity") {
      status = 200;
      body = { ok: true, messageCount: 0, latestMessageTs: 0, userCount: 0 };
    } else if (url.pathname === "/api/accounts/users") {
      status = 200;
      body = { ok: true, users: [{ name: "alice" }, { name: "bob" }] };
    } else if (url.pathname === "/api/chat/channels" && request.method() === "GET") {
      status = 200;
      body = { channels };
    } else if (url.pathname === `${channelPath}/members`) {
      status = 200;
      body = { members: [{ username: "bob", joinedAt: Date.now() }] };
    } else if (url.pathname === `${channelPath}/webhooks`) {
      status = 200;
      body = { webhooks: [] };
    } else if (url.pathname === `${channelPath}/room-access` && channels.length) {
      status = 200;
      body = {
        channel: channels[0],
        room: `chat-channel:${channelId}:v1`,
        passphrase: "private-key",
        webSocketUrl: `${channelPath}/ws?ticket=alice-ticket`,
      };
    } else if (url.pathname === channelPath && request.method() === "PATCH") {
      const data = request.postDataJSON();
      channels[0] = {
        ...channels[0],
        name: data.name,
        topic: data.topic,
        visibility: data.visibility,
        updatedAt: Date.now(),
      };
      status = 200;
      body = { ok: true, channel: channels[0] };
    } else if (url.pathname === channelPath && request.method() === "DELETE") {
      channels.length = 0;
      status = 200;
      body = { ok: true, removed: true };
    }
    await route.fulfill({
      status,
      contentType: "application/json",
      body: JSON.stringify(body),
    });
  });
  await page.routeWebSocket(
    "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
    () => {},
  );
  await page.routeWebSocket("**/api/chat/channels/*/ws?ticket=*", () => {});

  await page.goto("/chat.html");
  await page.getByRole("button", {
    name: "Private channel #release-team",
  }).click();
  await expect(page.locator("#chat-channel-title")).toHaveText("#release-team");

  await page.locator("#chat-channel-manage").click();
  await expect(page.locator("#chat-channel-settings")).toBeVisible();
  await page.locator("#chat-channel-settings-name").fill("shipping");
  await page.locator("#chat-channel-settings-topic").fill("Ship it");
  await page.getByRole("button", { name: "Save changes" }).click();
  await expect(page.locator("#chat-channel-title")).toHaveText("#shipping");
  await expect(page.locator("#chat-channel-topic")).toHaveText("Ship it");
  await expect(page.getByRole("button", { name: "Private channel #shipping" })).toContainText("shipping");

  await page.getByRole("button", { name: "Delete channel" }).click();
  await page.getByRole("button", { name: "Yes, delete" }).click();
  await expect(page.locator("#chat-channel-settings")).toBeHidden();
  await expect(page.locator("#chat-channel-title")).toHaveText("World");
  await expect(page.locator("#chat-rooms")).not.toContainText("shipping");
  await expect(page.locator("#chat-log")).toContainText(
    "Channel #shipping was deleted",
  );
});

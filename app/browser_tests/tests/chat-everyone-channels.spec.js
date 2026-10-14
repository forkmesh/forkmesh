const { test, expect } = require("@playwright/test");
const crypto = require("node:crypto");

const everyoneId = (name) => crypto.createHash("sha256")
  .update(`forkmesh-everyone-channel-v1:${name}`)
  .digest("hex")
  .slice(0, 32);

test("members land in server #general and the relay room is World", async ({ page }) => {
  const everyone = ["general", "random", "welcome"].map((name) => ({
    id: everyoneId(name),
    name,
    kind: "everyone",
    visibility: "public",
    canManage: false,
    memberCount: 3,
    topic: "",
    keyVersion: 1,
    updatedAt: 0,
  }));
  const team = {
    id: "d".repeat(32),
    name: "alpha",
    visibility: "public",
    canManage: false,
    keyVersion: 1,
    updatedAt: 0,
  };
  const memberRequests = [];

  await page.addInitScript(() => {
    if (!localStorage.getItem("forkmesh.session")) {
      localStorage.setItem("forkmesh.session", JSON.stringify({
        kind: "user",
        nodeName: "alice",
        email: "alice@example.test",
        sessionToken: "alice-token",
      }));
    }
  });

  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const authorization = request.headers().authorization || "";
    const actor = authorization.replace(/^Bearer\s+/i, "").replace(/-token$/, "");
    let status = 404;
    let body = { error: "not_found" };
    const channel = [...everyone, team].find((value) =>
      url.pathname.startsWith(`/api/chat/channels/${value.id}/`));

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
        users: ["alice", "bob", "carol", "dave"].map((name) => ({
          name,
          createdAt: Date.now() - 60_000,
        })),
      };
    } else if (url.pathname === "/api/chat/activity") {
      status = 200;
      body = { ok: true, messageCount: 0, latestMessageTs: 0, userCount: 0 };
    } else if (url.pathname === "/api/chat/channels" && request.method() === "GET") {
      if (actor) {
        status = 200;
        body = { channels: [team, ...everyone] };
      } else {
        status = 401;
        body = { error: "invalid_session" };
      }
    } else if (channel && url.pathname.endsWith("/members") && actor) {
      memberRequests.push(channel.name);
      status = 200;
      body = {
        everyone: true,
        members: ["alice", "bob", "carol"].map((username) => ({
          username,
          joinedAt: 1,
        })),
      };
    } else if (channel && url.pathname.endsWith("/room-access") && actor) {
      status = 200;
      body = {
        channel,
        room: `chat-channel:${channel.id}:v1`,
        passphrase: `playwright-${channel.name}-passphrase`,
        webSocketUrl: `/api/chat/channels/${channel.id}/ws?ticket=${actor}-ticket`,
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
  await expect(page.locator("#chat-channel-title")).toHaveText("#general");
  await expect(page.locator("#chat-rooms .chat-room-name")).toHaveText([
    "general",
    "random",
    "welcome",
    "World",
    "alpha",
  ]);
  await expect(page.locator(
    `#chat-rooms [data-channel="private:${everyone[0].id}"]`,
  )).toHaveAttribute("title", "Every ForkMesh member is in this channel");
  await expect(page.locator("#chat-rooms .chat-room-settings")).toHaveCount(0);
  await expect(page.locator("#chat-channel-manage")).toBeHidden();
  await expect.poll(() => memberRequests).toContain("general");
  await expect(page.locator("#chat-people-title")).toHaveText("Members · 3");
  await expect(page.locator("#chat-people")).toContainText("bob");
  await expect(page.locator("#chat-people")).not.toContainText("dave");

  await page.getByRole("button", { name: "World chat, open to visitors" }).click();
  await expect(page.locator("#chat-channel-title")).toHaveText("World");
  await page.reload();
  await expect(page.locator("#chat-channel-title")).toHaveText("#general");

  await page.getByRole("button", {
    name: "Every ForkMesh member is in this channel #welcome",
  }).click();
  await expect(page.locator("#chat-channel-title")).toHaveText("#welcome");
  await page.reload();
  await expect(page.locator("#chat-channel-title")).toHaveText("#welcome");

  await page.evaluate(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({ kind: "guest" }));
  });
  await page.reload();
  await expect(page.locator("#chat-channel-title")).toHaveText("World");
  await expect(page.locator("#chat-rooms .chat-room-name")).toHaveText(["World"]);
});

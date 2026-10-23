const { test, expect } = require("@playwright/test");
const crypto = require("node:crypto");

const everyoneId = (name) => crypto.createHash("sha256")
  .update(`forkmesh-everyone-channel-v1:${name}`)
  .digest("hex")
  .slice(0, 32);

test("the /chat people pane shows each member's activity line", async ({ page }) => {
  const everyone = ["general", "random", "welcome"].map((name) => ({
    id: everyoneId(name),
    name,
    kind: "everyone",
    visibility: "public",
    canManage: false,
    memberCount: 4,
    topic: "",
    keyVersion: 1,
    updatedAt: 0,
  }));
  const today = new Date().toISOString().slice(0, 10);
  const statsRequests = [];

  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      kind: "user",
      nodeName: "alice",
      email: "alice@example.test",
      sessionToken: "alice-token",
    }));
  });

  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    let status = 404;
    let body = { error: "not_found" };
    const channel = everyone.find((value) =>
      url.pathname.startsWith(`/api/chat/channels/${value.id}/`));

    if (url.pathname === "/api/chat/room-key") {
      status = 200;
      body = {
        ok: true,
        room: "world-general",
        access: "public-world-general",
        passphrase: "playwright-public-room-passphrase",
      };
    } else if (url.pathname === "/api/chat/channels" && request.method() === "GET") {
      status = 200;
      body = { channels: everyone };
    } else if (channel && url.pathname.endsWith("/members")) {
      status = 200;
      body = {
        everyone: true,
        members: ["alice", "bob", "carol", "build-bot"].map((username) => ({
          username,
          joinedAt: 1,
        })),
      };
    } else if (channel && url.pathname.endsWith("/room-access")) {
      status = 200;
      body = {
        channel,
        room: `chat-channel:${channel.id}:v1`,
        passphrase: `playwright-${channel.name}-passphrase`,
        webSocketUrl: `/api/chat/channels/${channel.id}/ws?ticket=alice-ticket`,
      };
    } else if (url.pathname === "/api/chat/member-stats") {
      statsRequests.push(url.searchParams.get("names"));
      status = 200;
      body = {
        ok: true,
        members: [
          {
            name: "bob",
            pulls: 3,
            commits: 1200,
            issues: 0,
            reviews: 1,
            activeDays: 12,
            lastActive: today,
          },
        ],
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
  await expect(page.locator("#chat-people-title")).toHaveText("Members · 4");

  const bob = page.locator('#chat-people .chat-person-stats[data-member-stats="bob"]');
  await expect(bob).toBeVisible();
  await expect(bob.locator(".chat-person-stat")).toHaveText(["3", "1.2k", "1", "today"]);
  await expect(bob).toHaveAttribute(
    "title",
    "Past year, public repositories: 3 pull requests opened, 1,200 commits, "
      + "1 review, active on 12 days, last contribution today",
  );
  await expect(bob.locator(".chat-visually-hidden")).toHaveText(
    await bob.getAttribute("title"),
  );
  await expect(
    page.locator('#chat-people .chat-person-stats[data-member-stats="carol"]'),
  ).toBeHidden();
  await expect(
    page.locator('#chat-people .chat-person-stats[data-member-stats="build-bot"]'),
  ).toHaveCount(0);

  expect(statsRequests).toHaveLength(1);
  expect(statsRequests[0].split(",")).not.toContain("build-bot");
  expect(statsRequests[0].split(",")).toContain("bob");

  await page.locator("#chat-people-search").fill("bo");
  await expect(bob).toBeVisible();
  expect(statsRequests).toHaveLength(1);
});

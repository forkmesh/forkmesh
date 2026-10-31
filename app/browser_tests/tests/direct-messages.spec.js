const { test, expect } = require("@playwright/test");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");

const RETAINED_KEY_ADVERTISEMENTS = 1;


test.setTimeout(90_000);


async function api(
  request,
  path,
  { token = "", method = "GET", data, requestHeaders = {} } = {},
) {
  const headers = { accept: "application/json", ...requestHeaders };
  if (token) headers.authorization = `Bearer ${token}`;
  const options = { method, headers };
  if (data !== undefined) options.data = data;
  const response = await request.fetch(path, options);
  return { status: response.status(), body: await response.json() };
}


async function signup(request, name, sourceAddress) {
  // Follow the same page-owned flow as a real browser. GET /signup stores the
  // one-use HttpOnly claim in this request context and renders the temporary
  // visible email field name; the conventional `email` key is the honeypot.
  const signupPage = await request.get("/signup");
  expect(signupPage.status()).toBe(200);
  const signupHtml = await signupPage.text();
  const emailField = signupHtml.match(
    /id="acct-email"\s+name="(contact_[0-9a-f]{18})"/,
  )?.[1];
  expect(emailField).toBeTruthy();
  const result = await api(request, "/api/accounts/signup", {
    method: "POST",
    data: {
      nodeName: name,
      email: "",
      [emailField]: `${name}@example.test`,
      password: "correct-horse-battery-staple", // forkmesh-secret-scan:ignore-line -- test-only credential
      // Local dev answers Cloudflare's Turnstile test keys in process.
      turnstileToken: "XXXX.DUMMY.TOKEN.XXXX",
    },
    requestHeaders: { "cf-connecting-ip": sourceAddress },
  });
  expect(result.status, JSON.stringify(result.body)).toBe(201);
  await confirmEmail(request, result.body.nodeName, `${name}@example.test`);
  return { ...result.body, emailVerified: true };
}


function browserTestDataKey() {
  const config = fs.readFileSync(
    path.resolve(__dirname, "..", "..", "..", "world", "browser_tests", "wrangler.browser-tests.toml"),
    "utf8",
  );
  return config.match(/^DATA_KEY\s*=\s*"([^"]+)"/m)[1];
}


async function confirmEmail(request, nodeName, email) {
  const issuedAt = Date.now();
  const key = crypto.createHash("sha256").update(`${browserTestDataKey()}:blind-index`).digest();
  const mac = crypto.createHmac("sha256", key)
    .update(`forkmesh-email-verify-v3\n${nodeName}\n${email}\n${issuedAt}`)
    .digest();
  const stamp = Buffer.alloc(6);
  stamp.writeUIntBE(issuedAt, 0, 6);
  const token = Buffer.concat([stamp, mac]).toString("base64url");
  const response = await request.post("/api/accounts/verify-email", {
    form: { node: nodeName, token },
  });
  expect(await response.text()).toContain("Email confirmed");
}


function browserSession(account) {
  return {
    kind: "user",
    nodeName: account.nodeName,
    email: account.email,
    sessionToken: account.sessionToken,
    isAdmin: Boolean(account.isAdmin),
    status: account.status,
  };
}


async function accountPage(browser, account) {
  const context = await browser.newContext();
  await context.addInitScript((session) => {
    localStorage.setItem("forkmesh.session", JSON.stringify(session));
  }, browserSession(account));
  return { context, page: await context.newPage() };
}


test("suggests registered people after typing an at-sign in the chat composer", async ({
  browser,
  request,
}) => {
  const suffix = `${Date.now().toString(36)}${Math.random().toString(36).slice(2, 7)}`;
  const aliceName = `mentionalice${suffix}`.slice(0, 30);
  const bobName = `mentionbob${suffix}`.slice(0, 30);
  const sourceSubnet = 1 + Math.floor(Math.random() * 250);
  const alice = await signup(request, aliceName, `198.18.${sourceSubnet}.11`);
  await signup(request, bobName, `198.18.${sourceSubnet}.12`);

  const aliceBrowser = await accountPage(browser, alice);
  await aliceBrowser.page.goto("/chat.html");
  const input = aliceBrowser.page.locator("#chat-input");
  await input.fill(`@${bobName}`);
  const suggestion = aliceBrowser.page.locator(".chat-mention-suggest-item", {
    hasText: bobName,
  });
  await expect(suggestion).toBeVisible();
  await input.press("Tab");
  await expect(input).toHaveValue(`@${bobName} `);

  // Pointer selection has to insert the name too, not just Tab. Hovering a row
  // used to re-render the whole list, so the row under the cursor was detached
  // and rebuilt every frame and the press landed on a node that was already
  // gone, leaving the composer on a bare "@".
  await input.fill(`@${bobName}`);
  await expect(suggestion).toBeVisible();
  await suggestion.hover();
  await suggestion.click();
  await expect(input).toHaveValue(`@${bobName} `);
  await expect(input).toBeFocused();
  await expect(aliceBrowser.page.locator(".chat-mention-suggest")).toBeHidden();

  await input.press("Shift+Enter");
  await input.type("follow-up");
  await expect(input).toHaveValue(`@${bobName} \nfollow-up`);

  await expect(aliceBrowser.page.locator("#chat-status")).toContainText(
    "Connected",
  );
  await input.fill("Enter sends this message");
  await input.press("Enter");
  await expect(input).toHaveValue("");

  await aliceBrowser.context.close();
});


test("real participants create, replay, and isolate an encrypted direct message", async ({
  browser,
  request,
}) => {
  const suffix = `${Date.now().toString(36)}${Math.random().toString(36).slice(2, 7)}`;
  const aliceName = `alice${suffix}`.slice(0, 30);
  const bobName = `bob${suffix}`.slice(0, 30);
  const outsiderName = `outsider${suffix}`.slice(0, 30);
  const sourceSubnet = 1 + Math.floor(Math.random() * 250);
  const alice = await signup(request, aliceName, `198.18.${sourceSubnet}.1`);
  const bob = await signup(request, bobName, `198.18.${sourceSubnet}.2`);
  const outsider = await signup(
    request,
    outsiderName,
    `198.18.${sourceSubnet}.3`,
  );

  const privateProfile = await api(request, "/api/accounts/profile", {
    token: outsider.sessionToken,
    method: "POST",
    data: { profilePrivate: true },
  });
  expect(privateProfile.status).toBe(200);
  const publicDirectory = await api(request, "/api/accounts/users");
  expect(publicDirectory.status).toBe(200);
  expect(publicDirectory.body.users.map((user) => user.name)).not.toContain(
    outsiderName,
  );
  const directSearch = await api(
    request,
    `/api/chat/direct-messages/users?query=${encodeURIComponent(outsiderName)}`,
    { token: alice.sessionToken },
  );
  expect(directSearch.status).toBe(200);
  expect(directSearch.body.users.map((user) => user.name)).not.toContain(
    outsiderName,
  );

  const aliceBrowser = await accountPage(browser, alice);
  const retainedFrames = [];
  aliceBrowser.page.on("websocket", (socket) => {
    if (!socket.url().includes("/api/chat/direct-messages/")) return;
    socket.on("framesent", (event) => {
      const frame = String(event.payload);
      if (JSON.parse(frame).persist === true) retainedFrames.push(frame);
    });
  });
  await aliceBrowser.page.goto("/chat.html");
  await aliceBrowser.page.getByRole("button", {
    name: "New direct message",
  }).click();
  await aliceBrowser.page.locator("#chat-direct-search").fill(bobName);
  await aliceBrowser.page.locator("#chat-direct-dialog").getByRole("button", {
    name: `Message ${bobName}`,
    exact: true,
  }).click();

  await expect(aliceBrowser.page.locator("#chat-channel-title")).toHaveText(
    `@${bobName}`,
  );
  await expect(aliceBrowser.page.locator("#chat-status")).toContainText(
    "Connected",
  );
  await aliceBrowser.page.locator("#chat-input").fill("First private message");
  await aliceBrowser.page.locator("#chat-send").click();
  await aliceBrowser.page.locator("#chat-input").fill("Second private message");
  await aliceBrowser.page.locator("#chat-send").click();
  const directRoot = aliceBrowser.page.locator("#chat-log .chat-msg", {
    hasText: "First private message",
  });
  await directRoot.hover();
  await directRoot.getByRole("button", { name: "Reply in thread" }).click();
  await aliceBrowser.page.locator("#chat-thread-input").fill(
    "Private thread reply",
  );
  await aliceBrowser.page.locator("#chat-thread-send").click();
  await expect(aliceBrowser.page.locator("#chat-thread-replies")).toContainText(
    "Private thread reply",
  );
  const directReply = aliceBrowser.page.locator(
    "#chat-thread-replies .chat-thread-message",
    { hasText: "Private thread reply" },
  );
  await directReply.hover();
  await directReply.getByRole("button", { name: "Delete message" }).click();
  await aliceBrowser.page.locator("#chat-delete-confirm").click();
  await expect(aliceBrowser.page.locator("#chat-thread-replies")).not.toContainText(
    "Private thread reply",
  );
  await aliceBrowser.page.locator("#chat-attachment-input").setInputFiles({
    name: "private-note.txt",
    mimeType: "text/plain",
    buffer: Buffer.from("private attachment body"),
  });
  await expect.poll(() => retainedFrames.length).toBe(
    RETAINED_KEY_ADVERTISEMENTS + 5,
  );
  expect(retainedFrames.join("\n")).not.toContain("First private message");
  expect(retainedFrames.join("\n")).not.toContain("Second private message");
  expect(retainedFrames.join("\n")).not.toContain("private attachment body");

  let bobConversations;
  await expect.poll(async () => {
    bobConversations = await api(request, "/api/chat/direct-messages", {
      token: bob.sessionToken,
    });
    return bobConversations.body.conversations?.[0]?.unreadCount;
  }).toBe(4);
  const conversation = bobConversations.body.conversations[0];

  const reopened = await api(request, "/api/chat/direct-messages", {
    token: bob.sessionToken,
    method: "POST",
    data: { username: aliceName },
  });
  expect(reopened.status).toBe(200);
  expect(reopened.body.conversation.id).toBe(conversation.id);

  const denied = await api(
    request,
    `/api/chat/direct-messages/${conversation.id}/room-access`,
    { token: outsider.sessionToken },
  );
  expect(denied).toEqual({ status: 404, body: { error: "not_found" } });

  const bobBrowser = await accountPage(browser, bob);
  await bobBrowser.page.goto("/chat.html");
  await expect(
    bobBrowser.page.getByRole("button", {
      name: `Direct message with ${aliceName}`,
    }).locator(".chat-room-unread"),
  ).toHaveText("4");
  await bobBrowser.page.getByRole("button", {
    name: `Direct message with ${aliceName}`,
  }).click();
  await expect(bobBrowser.page.locator("#chat-log")).toContainText(
    "First private message",
  );
  await expect(bobBrowser.page.locator("#chat-log")).toContainText(
    "Second private message",
  );
  await expect(bobBrowser.page.locator("#chat-log")).toContainText(
    "private-note.txt",
  );
  const receivedDirectRoot = bobBrowser.page.locator("#chat-log .chat-msg", {
    hasText: "First private message",
  });
  await receivedDirectRoot.hover();
  await receivedDirectRoot.getByRole("button", {
    name: "Reply in thread",
  }).click();
  await expect(bobBrowser.page.locator("#chat-thread-replies")).not.toContainText(
    "Private thread reply",
  );
  await expect(
    bobBrowser.page.locator("#chat-direct-list .chat-room-unread"),
  ).toHaveCount(0);

  await bobBrowser.page.setViewportSize({ width: 390, height: 844 });
  const mobileLayout = await bobBrowser.page.evaluate(() => ({
    innerWidth: window.innerWidth,
    scrollWidth: document.documentElement.scrollWidth,
    offenders: [...document.querySelectorAll("body *")]
      .map((element) => ({
        tag: element.tagName.toLowerCase(),
        id: element.id,
        className: String(element.className || ""),
        left: Math.round(element.getBoundingClientRect().left),
        right: Math.round(element.getBoundingClientRect().right),
      }))
      .filter((item) => item.right > window.innerWidth + 1 || item.left < -1)
      .slice(0, 12),
  }));
  expect(mobileLayout.scrollWidth, JSON.stringify(mobileLayout, null, 2))
    .toBeLessThanOrEqual(mobileLayout.innerWidth);

  await aliceBrowser.context.close();
  await bobBrowser.context.close();
});

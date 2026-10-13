const { test, expect } = require("@playwright/test");
const { createCipheriv, createHash, pbkdf2Sync } = require("node:crypto");
const OFFICE_ENTRY_TICKET = "playwright-office-entry-ticket";

const REMOTE_PARTICIPANT = {
  id: "participant-remote",
  name: "Remote teammate",
  accountStatus: "Registered",
  x: 1.5,
  y: 0.38,
  z: -1,
  yaw: 0,
  moving: false,
  pose: "standing",
  chairId: "",
  bindingKey: null,
  updatedAt: 1785000000000,
};

function encryptChatEnvelope(message) {
  const salt = createHash("sha256")
    .update("ForkMesh room:world-general", "utf8")
    .digest()
    .subarray(0, 16);
  const key = pbkdf2Sync(
    "playwright-office-meeting-passphrase",
    salt,
    210000,
    32,
    "sha256",
  );
  const nonce = Buffer.alloc(12, 9);
  const cipher = createCipheriv("aes-256-gcm", key, nonce);
  const body = Buffer.concat([
    cipher.update(JSON.stringify(message), "utf8"),
    cipher.final(),
  ]);
  return {
    kind: "cipher",
    v: 1,
    nonce: nonce.toString("base64"),
    tag: cipher.getAuthTag().toString("base64"),
    body: body.toString("base64"),
  };
}

async function prepareMeetingWorld(page) {
  const chatFrames = [];
  const meetingFrames = [];
  const officeEntryRequests = [];
  const generalAccessHeaders = [];
  let chatSocket = null;
  let meetingSocket = null;
  await page.addInitScript(() => {
    Date.now = () => 1785000000000;
    let seed = 0x0ff1ce;
    Math.random = () => {
      seed = (seed * 1664525 + 1013904223) >>> 0;
      return seed / 0x1_0000_0000;
    };
    sessionStorage.setItem("forkmesh.world.guestId.v1", "office-meeting");
    localStorage.setItem("forkmesh.session", JSON.stringify({
      kind: "user",
      nodeName: "alice",
      email: "alice@example.test",
      sessionToken: "alice-office-token",
    }));
  });
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    let status = 200;
    let body = {};
    if (url.pathname === "/api/world/context") {
      body = { now: 1785000000000, countryCode: "" };
    } else if (url.pathname === "/api/world/ticket") {
      body = {
        authenticated: true,
        accountStatus: "Registered",
        name: "alice",
        isAdmin: false,
        nodeCount: 0,
        ticket: "playwright-world-ticket",
        expiresAt: 1785000060000,
        totalActiveMs: 0,
        activityObservedAt: 1785000000000,
      };
    } else if (url.pathname === "/api/world/instances") {
      body = { instances: [] };
    } else if (url.pathname === "/api/world/fediverse") {
      body = { mastodon: [], lemmy: [], x: [], reddit: [] };
    } else if (url.pathname === "/api/world/media/spaces") {
      body = { spaces: [] };
    } else if (url.pathname === "/api/repositories") {
      body = { repositories: [] };
    } else if (url.pathname === "/api/chat/channels") {
      status = 401;
      body = { error: "unauthorized" };
    } else if (url.pathname === "/api/chat/room-key") {
      body = {
        ok: true,
        room: "world-general",
        passphrase: "playwright-office-meeting-passphrase",
      };
    } else if (url.pathname === "/api/world/office/floors") {
      body = {
        authenticated: true,
        account: "alice",
        allowedFloorIds: ["lobby", "marketing", "rooftop"],
        teams: [],
      };
    } else if (url.pathname === "/api/world/office/general/entry") {
      let requestBody = {};
      try {
        requestBody = route.request().postDataJSON();
      } catch (_) {}
      officeEntryRequests.push({
        method: route.request().method(),
        url: route.request().url(),
        body: requestBody,
      });
      body = {
        ok: true,
        occupied: false,
        entryTicket: OFFICE_ENTRY_TICKET,
        expiresAt: 1785000060000,
      };
    } else if (url.pathname === "/api/world/office/general/access") {
      generalAccessHeaders.push(route.request().headers());
      body = {
        room: { id: "general", name: "general", visibility: "public" },
        meetingWebSocketUrl:
          "/api/world/office/general/ws?ticket=playwright-office-ticket",
        expiresAt: 1785000060000,
      };
    }
    return route.fulfill({
      status,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.routeWebSocket("**/api/world/ws*", (socket) => {
    socket.send(JSON.stringify({ type: "welcome", id: "world-self", peers: [] }));
  });
  await page.routeWebSocket("**/api/world/office/general/ws*", (socket) => {
    meetingSocket = socket;
    socket.send(JSON.stringify({
      type: "welcome",
      id: "participant-self",
      participants: [REMOTE_PARTICIPANT],
    }));
    socket.onMessage((raw) => {
      const frame = JSON.parse(String(raw));
      meetingFrames.push(frame);
      if (frame.type !== "seat-request") return;
      socket.send(JSON.stringify({
        type: "seat",
        participant: {
          ...REMOTE_PARTICIPANT,
          id: "participant-self",
          name: "Alice",
          pose: "seated",
          chairId: frame.chairId,
        },
      }));
    });
  });
  await page.routeWebSocket(
    "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
    (socket) => {
      chatSocket = socket;
      socket.onMessage((raw) => chatFrames.push(JSON.parse(String(raw))));
    },
  );

  await page.goto("/world/");
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return (
      Boolean(shell?.world?.renderer?.domElement) &&
      Number(shell.world.renderer.info?.render?.frame || 0) > 0
    );
  });
  await page.waitForFunction(
    () => !document.querySelector("[data-world-loading]"),
  );
  return {
    chatFrames,
    meetingFrames,
    officeEntryRequests,
    generalAccessHeaders,
    sendChatFrame: (plain) => chatSocket.send(JSON.stringify(encryptChatEnvelope(plain))),
    closeMeeting: (code = 1008) => meetingSocket.close({
      code,
      reason: "room access revoked",
      wasClean: true,
    }),
  };
}

async function waitForOfficeEntry(page) {
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate((shell) => ({
      active: shell.officeController?.active === true,
      space: shell.world.getPosition().space,
    }))
  ).toEqual({
    active: true,
    space: "office-lobby",
  });
}

async function walkIntoOffice(page) {
  const entry = await page.locator("forkmesh-world").evaluate(async (shell) => {
    shell.world.setPaused(true);
    shell.world.player.position.set(0, 0.38, -171);
    const controller = await shell.ensureOfficeRuntime();
    const entered = await controller.enterOffice({ source: "doorway" });
    return {
      entered,
      active: controller.active,
      space: shell.world.getPosition().space,
    };
  });
  expect(entry).toEqual({
    entered: true,
    active: true,
    space: "office-lobby",
  });
  await waitForOfficeEntry(page);
}

async function joinGeneralOfficeMeeting(page) {
  return page.locator("forkmesh-world").evaluate(
    (shell) => shell.officeMeeting.joinRoom("general"),
  );
}

test("signed-in visitors join a native Office meeting and sit after server approval", async ({ page }) => {
  const { meetingFrames, officeEntryRequests, generalAccessHeaders } =
    await prepareMeetingWorld(page);
  await walkIntoOffice(page);

  const lobby = page.locator("[data-world-office-lobby]");
  await expect(lobby).toBeHidden();
  await expect(page.locator(".world-quick-dock")).toBeHidden();
  await expect(page.locator("[data-world-office-frame]")).not.toHaveAttribute(
    "src",
    /.+/,
  );
  expect(officeEntryRequests).toHaveLength(0);
  expect(await joinGeneralOfficeMeeting(page)).toBe(true);
  expect(officeEntryRequests).toEqual([
    {
      method: "POST",
      url: expect.stringMatching(/\/api\/world\/office\/general\/entry$/),
      body: {},
    },
  ]);
  expect(generalAccessHeaders).toHaveLength(1);
  expect(generalAccessHeaders[0]["x-forkmesh-office-entry"]).toBe(
    OFFICE_ENTRY_TICKET,
  );

  const room = page.locator("[data-world-office-room]");
  await expect(room).toBeVisible();
  await expect(room).toContainText("#general");
  await expect(
    room.locator('[data-world-office-participant="participant-remote"]'),
  ).toContainText("Remote teammate");

  expect(await page.locator("forkmesh-world").evaluate((shell) =>
    shell.officeMeeting.move({
      x: 0.5,
      y: 0.38,
      z: 0,
      yaw: Math.PI / 2,
      moving: true,
    })
  )).toBe(true);
  await expect.poll(() => meetingFrames.some(
    (frame) => frame.type === "move" && frame.moving === true,
  )).toBe(true);

  await room.locator('[data-world-office-seat="chair-1"]').click();
  await expect(room.getByRole("button", { name: "Stand up" })).toBeVisible();
  await expect(room.locator('[data-world-office-seat="chair-1"]')).toHaveAttribute(
    "aria-pressed",
    "true",
  );
});

test("native Office messages, pasted images, and documents stay encrypted", async ({ page }) => {
  const { chatFrames, sendChatFrame } = await prepareMeetingWorld(page);
  await walkIntoOffice(page);
  expect(await joinGeneralOfficeMeeting(page)).toBe(true);

  const room = page.locator("[data-world-office-room]");
  const input = room.locator("[data-world-office-input]");
  await expect(input).toBeEnabled();
  await input.fill("A verified meeting message");
  await input.press("Enter");
  await expect(room.locator("[data-world-office-transcript]")).toContainText(
    "A verified meeting message",
  );
  await expect(page.locator('[data-world-office-bubble="participant-self"]'))
    .toContainText("A verified meeting message");

  sendChatFrame({
    type: "chat",
    id: "forged-remote-message",
    senderId: "forged-remote-sender",
    sender: "Remote teammate",
    channel: "#general",
    text: "Tampered avatar claim",
    ts: 1785000000000,
    meetingProof: {
      v: 1,
      participantId: "participant-remote",
      ts: 1785000000000,
      signature: "not-a-valid-signature",
    },
  });
  await input.evaluate((element) => {
    const png = Uint8Array.from(atob(
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=",
    ), (char) => char.charCodeAt(0));
    const transfer = new DataTransfer();
    transfer.items.add(new File([png], "office-paste.png", { type: "image/png" }));
    element.dispatchEvent(new ClipboardEvent("paste", {
      bubbles: true,
      cancelable: true,
      clipboardData: transfer,
    }));
  });
  await expect(room.locator(".world-office-transcript-image")).toHaveAttribute(
    "alt",
    "office-paste.png",
  );
  await room.locator("[data-world-office-attachment-input]").setInputFiles({
    name: "meeting-notes.pdf",
    mimeType: "application/pdf",
    buffer: Buffer.from("private meeting document"),
  });
  await expect(room.locator(".world-office-transcript-file")).toContainText(
    "meeting-notes.pdf",
  );
  await expect(room.locator("[data-world-office-transcript]")).not.toContainText(
    "Tampered avatar claim",
  );
  await expect(page.locator('[data-world-office-bubble="participant-remote"]'))
    .toHaveCount(0);

  await expect.poll(
    () => chatFrames.filter((frame) => frame.persist === true).length,
  ).toBe(3);
  const wire = JSON.stringify(chatFrames);
  expect(wire).not.toContain("A verified meeting message");
  expect(wire).not.toContain("office-paste.png");
  expect(wire).not.toContain("meeting-notes.pdf");
});

test("meeting revocation clears the room and returns to the physical Marketing floor", async ({ page }) => {
  const { closeMeeting } = await prepareMeetingWorld(page);
  await walkIntoOffice(page);
  expect(await joinGeneralOfficeMeeting(page)).toBe(true);
  const room = page.locator("[data-world-office-room]");
  await expect(room).toBeVisible();
  await room.locator('[data-world-office-seat="chair-1"]').click();
  await expect(room.getByRole("button", { name: "Stand up" })).toBeVisible();

  closeMeeting();
  await expect(room).toBeHidden();
  await expect(page.locator("[data-world-office-lobby]")).toBeHidden();
  await expect(page.locator("[data-world-office-lobby-status]")).toContainText(
    "meeting ended",
  );
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate(
      (shell) => shell.world.getPosition().space,
    )
  ).toBe("office-marketing");
  await expect(room.locator("[data-world-office-participant]")).toHaveCount(0);
  await expect(page.locator("[data-world-office-bubble]")).toHaveCount(0);
});

for (const viewport of [
  { name: "desktop", options: { viewport: { width: 1440, height: 900 } } },
  {
    name: "portrait",
    options: {
      viewport: { width: 390, height: 844 },
      hasTouch: true,
      isMobile: true,
    },
  },
  {
    name: "landscape",
    options: {
      viewport: { width: 844, height: 390 },
      hasTouch: true,
      isMobile: true,
    },
  },
  {
    name: "narrow",
    options: {
      viewport: { width: 320, height: 568 },
      hasTouch: true,
      isMobile: true,
    },
  },
]) {
  test(`Office meeting visual acceptance ${viewport.name}`, async ({ browser }) => {
    const context = await browser.newContext({
      ...viewport.options,
      timezoneId: "UTC",
    });
    const page = await context.newPage();
    await prepareMeetingWorld(page);
    await walkIntoOffice(page);
    expect(await joinGeneralOfficeMeeting(page)).toBe(true);
    const room = page.locator("[data-world-office-room]");
    const input = room.locator("[data-world-office-input]");
    await expect(input).toBeEnabled();
    await input.fill("Design review starts in five minutes.");
    await input.press("Enter");
    await room.locator('[data-world-office-seat="chair-1"]').click();
    await expect(room.getByRole("button", { name: "Stand up" })).toBeVisible();
    await room.evaluate((element) => {
      element.scrollTop = element.scrollHeight;
    });
    await expect(room.locator("[data-world-office-input]")).toBeVisible();
    await expect(room.getByRole("button", { name: "Leave ForkMesh Office" }))
      .toBeVisible();
    await page.locator("forkmesh-world").evaluate((shell) => {
      shell.world.setPaused(true);
    });
    await page.evaluate(() => document.fonts?.ready);
    expect(await page.evaluate(
      () => document.documentElement.scrollWidth <= window.innerWidth,
    )).toBe(true);
    const layout = await room.evaluate((element) => {
      const bounds = (node) => {
        const rect = node.getBoundingClientRect();
        return {
          left: rect.left,
          top: rect.top,
          right: rect.right,
          bottom: rect.bottom,
          width: rect.width,
          height: rect.height,
        };
      };
      const style = getComputedStyle(element);
      return {
        viewport: { width: window.innerWidth, height: window.innerHeight },
        panel: bounds(element),
        input: bounds(element.querySelector("[data-world-office-input]")),
        exit: bounds(element.querySelector("[data-world-office-exit]")),
        open: element.dataset.open,
        ariaHidden: element.getAttribute("aria-hidden"),
        visibility: style.visibility,
        pointerEvents: style.pointerEvents,
        scrollWidth: element.scrollWidth,
        clientWidth: element.clientWidth,
      };
    });
    expect(layout.open).toBe("true");
    expect(layout.ariaHidden).toBe("false");
    expect(layout.visibility).toBe("visible");
    expect(layout.pointerEvents).toBe("auto");
    expect(layout.panel.left).toBeGreaterThanOrEqual(-1);
    expect(layout.panel.top).toBeGreaterThanOrEqual(-1);
    expect(layout.panel.right).toBeLessThanOrEqual(layout.viewport.width + 1);
    expect(layout.panel.bottom).toBeLessThanOrEqual(layout.viewport.height + 1);
    expect(layout.panel.width).toBeGreaterThanOrEqual(
      Math.min(300, layout.viewport.width),
    );
    expect(layout.panel.height).toBeGreaterThan(200);
    expect(layout.scrollWidth).toBeLessThanOrEqual(layout.clientWidth + 1);
    for (const control of [layout.input, layout.exit]) {
      expect(control.left).toBeGreaterThanOrEqual(layout.panel.left - 1);
      expect(control.right).toBeLessThanOrEqual(layout.panel.right + 1);
      expect(control.top).toBeGreaterThanOrEqual(-1);
      expect(control.bottom).toBeLessThanOrEqual(layout.viewport.height + 1);
    }
    if (layout.viewport.width <= 720) {
      expect(Math.abs(layout.panel.left)).toBeLessThanOrEqual(1);
      expect(Math.abs(layout.panel.right - layout.viewport.width))
        .toBeLessThanOrEqual(1);
      expect(Math.abs(layout.panel.bottom - layout.viewport.height))
        .toBeLessThanOrEqual(1);
    } else {
      expect(layout.panel.width).toBeLessThanOrEqual(721);
      expect(Math.abs(layout.panel.right - (layout.viewport.width - 20)))
        .toBeLessThanOrEqual(1);
      expect(layout.panel.top).toBeGreaterThanOrEqual(83);
    }
    await context.close();
  });
}

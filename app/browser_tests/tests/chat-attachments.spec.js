const { test, expect } = require("@playwright/test");

// A 64x64 VP8 clip (well under the 1 MiB attachment ceiling). It has to be a
// real, decodable video: an undecodable one would fire the player's error
// handler and be replaced by the plain download card.
const TINY_WEBM_BASE64 =
  "GkXfo59ChoEBQveBAULygQRC84EIQoKEd2VibUKHgQJChYECGFOAZwEAAAAAAAIeEU2bdLpNu4tT" +
  "q4QVSalmU6yBoU27i1OrhBZUrmtTrIHWTbuMU6uEElTDZ1OsggEjTbuMU6uEHFO7a1OsggII7AEA" +
  "AAAAAABZAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAVSalmsCrXsYMPQkBNgIxMYXZm" +
  "NjEuNy4xMDNXQYxMYXZmNjEuNy4xMDNEiYhAaQAAAAAAABZUrmvIrgEAAAAAAAA/14EBc8WI1Nht" +
  "Bj6+VeqcgQAitZyDdW5kiIEAhoVWX1ZQOIOBASPjg4QF9eEA4JCwgUC6gUCagQJVsIRVuYEBElTD" +
  "Z/tzc59jwIBnyJlFo4dFTkNPREVSRIeMTGF2ZjYxLjcuMTAzc3PWY8CLY8WI1NhtBj6+VepnyKFF" +
  "o4dFTkNPREVSRIeUTGF2YzYxLjE5LjEwMSBsaWJ2cHhnyKFFo4hEVVJBVElPTkSHkzAwOjAwOjAw" +
  "LjIwMDAwMDAwMAAfQ7Z14OeBAKPDgQAAgJADAJ0BKkAAQAAARwiFhYiFhIgCAgJ1qgP4AgboQVwx" +
  "0hMAVVgA/v6UgH/unr2nr2nr+YP/8Vv+K3/Fb/+JxKOWgQBkANEBAAEQEAAYABhYL/QACI6AABxT" +
  "u2uRu4+zgQC3iveBAfGCAaPwgQM=";

test("clipboard images and documents stay encrypted and survive refresh", async ({ page }) => {
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

  await page.locator("#chat-input").evaluate((input) => {
    const png = Uint8Array.from(atob(
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=",
    ), (char) => char.charCodeAt(0));
    const transfer = new DataTransfer();
    transfer.items.add(new File([png], "clipboard-image.png", { type: "image/png" }));
    input.dispatchEvent(new ClipboardEvent("paste", {
      bubbles: true,
      cancelable: true,
      clipboardData: transfer,
    }));
  });

  const image = page.locator(".chat-attachment-image");
  await expect(image).toBeVisible();
  await expect(image).toHaveAttribute("alt", "clipboard-image.png");

  const documentInput = page.locator("#chat-attachment-input");
  await documentInput.setInputFiles({
    name: "release-notes.txt",
    mimeType: "text/plain",
    buffer: Buffer.from("private release notes"),
  });

  const documentCard = page.locator(".chat-attachment-card", {
    hasText: "release-notes.txt",
  });
  await expect(documentCard).toBeVisible();
  await expect(documentCard.locator("a[download='release-notes.txt']")).toBeVisible();
  await expect.poll(() => retainedFrames.length).toBe(2);
  expect(retainedFrames.join("\n")).not.toContain("release-notes.txt");
  expect(retainedFrames.join("\n")).not.toContain("private release notes");

  await documentInput.setInputFiles({
    name: "too-large.bin",
    mimeType: "application/octet-stream",
    buffer: Buffer.alloc(1024 * 1024 + 1),
  });
  await expect(page.locator("#chat-attachment-feedback")).toContainText(
    "1 MiB or smaller",
  );
  await expect.poll(() => retainedFrames.length).toBe(2);

  // adhoc #72: a shared video plays where it was posted instead of arriving
  // as one more file to download first.
  await documentInput.setInputFiles({
    name: "screen-capture.webm",
    mimeType: "video/webm",
    buffer: Buffer.from(TINY_WEBM_BASE64, "base64"),
  });
  const player = page.locator(".chat-attachment-video");
  await expect(player).toBeVisible();
  await expect(player).toHaveJSProperty("controls", true);
  await expect(
    page.locator(".chat-attachment-video-preview a[download='screen-capture.webm']"),
  ).toBeVisible();
  // The element decoded the clip rather than falling back to the file card.
  await expect
    .poll(() => player.evaluate((video) => video.readyState))
    .toBeGreaterThan(0);
  await expect.poll(() => retainedFrames.length).toBe(3);
  expect(retainedFrames.join("\n")).not.toContain("screen-capture.webm");

  await page.reload();

  await expect(page.locator("#chat-status")).toContainText("Connected");
  await expect(page.locator(".chat-attachment-image")).toBeVisible();
  await expect(page.locator(".chat-attachment-card", {
    hasText: "release-notes.txt",
  })).toBeVisible();
  await expect(page.locator(".chat-attachment-video")).toBeVisible();
});

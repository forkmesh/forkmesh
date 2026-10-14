const { test, expect, chromium } = require("@playwright/test");
const { createHash } = require("node:crypto");

const CONVERSATION = "a".repeat(32);
const BASE = `/api/chat/direct-messages/${CONVERSATION}/media`;

function mediaServer() {
  const store = new Map();
  let next = 0;
  const calls = [];
  async function handle(route) {
    const request = route.request();
    const url = new URL(request.url());
    const method = request.method();
    calls.push(`${method} ${url.pathname.replace(/[0-9a-f]{32}/g, "ID")}`);
    const body = request.postData() ? JSON.parse(request.postData()) : null;
    const reply = (status, data) =>
      route.fulfill({ status, contentType: "application/json", body: JSON.stringify(data) });
    if (method === "POST" && url.pathname === BASE) {
      next += 1;
      const id = String(next).padStart(32, "0");
      store.set(id, { ...body, state: "uploading", parts: new Map() });
      return reply(201, { ok: true, media: { id, state: "uploading" } });
    }
    const match = url.pathname.match(/\/media\/([0-9a-f]{32})(?:\/(parts)\/(\d+)|\/(finish|consume))?$/);
    if (url.pathname === BASE && method === "GET") {
      const ids = String(url.searchParams.get("ids") || "").split(",");
      return reply(200, {
        media: ids.filter((id) => store.has(id)).map((id) => ({ id, state: store.get(id).state })),
      });
    }
    const item = match && store.get(match[1]);
    if (!item) return reply(404, { error: "not_found" });
    if (match[2] && method === "PUT") {
      item.parts.set(Number(match[3]), body.data);
      return reply(200, { ok: true });
    }
    if (match[2] && method === "GET") {
      if (item.state !== "ready") return reply(410, { error: "media_gone", state: item.state });
      return reply(200, { index: Number(match[3]), parts: item.parts.size, data: item.parts.get(Number(match[3])) });
    }
    if (match[4] === "finish") {
      item.state = "ready";
      return reply(200, { ok: true, media: { id: match[1], state: "ready" } });
    }
    if (match[4] === "consume") {
      item.parts.clear();
      item.state = "opened";
      return reply(200, { ok: true, media: { id: match[1], state: "opened" } });
    }
    return reply(405, { error: "method_not_allowed" });
  }
  return { store, calls, handle };
}

const HARNESS = `<!doctype html><html><head><meta charset="utf-8"><title>view once</title>
<link rel="stylesheet" href="/chat.css"></head><body><div id="log"></div>
<script type="module">
import * as viewOnce from "/chat-view-once.js";
import * as recorder from "/chat-view-once-recorder.js";
const request = async (path, options = {}) => {
  const response = await fetch(path, {
    ...options,
    headers: options.body ? { "content-type": "application/json" } : {},
  });
  const data = await response.json().catch(() => ({}));
  if (!response.ok) {
    const error = new Error(data.error || "failed");
    error.status = response.status;
    error.code = data.error;
    throw error;
  }
  return data;
};
const client = viewOnce.createViewOnceClient({ request });
window.harness = { viewOnce, recorder, client, revoked: [] };
const revoke = URL.revokeObjectURL.bind(URL);
URL.revokeObjectURL = (url) => { window.harness.revoked.push(url); revoke(url); };
window.harness.useVoices = async () => {
  const voice = await import("/chat-call-voice.js");
  recorder.registerViewOnceFilterStages({
    voice: (micTrack, voiceId) => voice.createVoiceStage({ sourceTrack: micTrack, voiceId }),
  });
  window.harness.voices = voice.CALL_VOICES.map((item) => [item.id, item.label]);
};
window.harness.record = (kind) => new Promise((resolve) => {
  recorder.openViewOnceRecorder({
    kind,
    recipient: "bob",
    voices: window.harness.voices || [],
    onSend: async (recording, onProgress) => {
      const sent = await client.upload("${CONVERSATION}", recording.kind, recording.blob,
        recording.durationMs, { mime: recording.mime, onProgress });
      window.harness.sentBytes = recording.blob.size;
      resolve({ type: "chat", text: viewOnce.VIEW_ONCE_KINDS[kind].text, viewOnce: sent });
    },
  });
});
window.harness.receive = (frame) => {
  const record = { id: "m1", self: false, viewOnce: viewOnce.viewOnceFromEntry(frame) };
  window.harness.received = record;
  const bubble = viewOnce.renderViewOnceBubble(record, {
    onOpen: async (target) => {
      target.viewOnceState = "opening";
      viewOnce.paintViewOnceBubble(target);
      const bytes = await client.download("${CONVERSATION}", target.viewOnce);
      viewOnce.openViewOnceViewer({
        kind: target.viewOnce.kind,
        bytes,
        mime: target.viewOnce.mime,
        onStart: () => {
          target.viewOnceState = "playing";
          viewOnce.paintViewOnceBubble(target);
          window.harness.consumed = client.consume("${CONVERSATION}", target.viewOnce.media);
        },
        onClose: async ({ started }) => {
          if (window.harness.consumed) await window.harness.consumed;
          target.viewOnceState = started ? "opened" : "ready";
          if (started) viewOnce.forgetViewOnceKey(target);
          viewOnce.paintViewOnceBubble(target);
        },
      });
    },
  });
  document.querySelector("#log").append(bubble);
};
window.harnessReady = true;
</script></body></html>`;

let browser;

test.beforeAll(async () => {
  browser = await chromium.launch({
    args: [
      "--use-fake-device-for-media-stream",
      "--use-fake-ui-for-media-stream",
      "--autoplay-policy=no-user-gesture-required",
    ],
  });
});

test.afterAll(async () => {
  await browser?.close();
});

for (const kind of ["video", "voice"]) {
  test(`records a ${kind} message, plays it once, and it is deleted`, async () => {
    test.setTimeout(60_000);
    const context = await browser.newContext({
      baseURL: "http://127.0.0.1:4179",
      permissions: ["camera", "microphone"],
    });
    const page = await context.newPage();
    const errors = [];
    page.on("pageerror", (error) => errors.push(error.message));
    const server = mediaServer();
    await page.route("**/api/chat/direct-messages/**", server.handle);
    await page.route("**/view-once-harness", (route) =>
      route.fulfill({ contentType: "text/html", body: HARNESS }));
    await page.goto("/view-once-harness");
    await page.waitForFunction(() => window.harnessReady === true);

    const framePromise = page.evaluate((value) => window.harness.record(value), kind);
    const dialog = page.getByRole("dialog");
    await expect(dialog).toBeVisible();
    await dialog.getByRole("button", { name: /Start recording/ }).click();
    await expect(dialog.getByRole("button", { name: "Stop recording" })).toBeVisible();
    await page.waitForTimeout(2500);
    await dialog.getByRole("button", { name: "Stop recording" }).click();
    await expect(dialog.getByRole("button", { name: /^Send / })).toBeVisible();
    await expect(dialog.locator(".chat-view-once-review")).toHaveCount(1);
    await expect(dialog.getByRole("button", { name: "Discard this recording" })).toBeVisible();
    await expect(dialog.getByRole("button", { name: "Record again" })).toBeVisible();
    await dialog.getByRole("button", { name: /^Send / }).click();
    const frame = await framePromise;
    await expect(dialog).toHaveCount(0);

    expect(frame.viewOnce.kind).toBe(kind);
    expect(frame.viewOnce.durationMs).toBeGreaterThan(1500);
    const [mediaId] = [...server.store.keys()];
    const stored = Buffer.concat(
      [...server.store.get(mediaId).parts.values()].map((part) => Buffer.from(part, "base64")));
    expect(stored.length).toBe(frame.viewOnce.size);
    expect(stored.subarray(0, 4).equals(Buffer.from([0x1a, 0x45, 0xdf, 0xa3]))).toBe(false);
    expect(server.store.get(mediaId).state).toBe("ready");

    await page.evaluate((value) => window.harness.receive(value), frame);
    const bubble = page.locator(".chat-view-once");
    await expect(bubble).toHaveAttribute(
      "aria-label", new RegExp(`Play ${kind} message, .*It can be played once`));
    await bubble.click();
    const viewer = page.getByRole("dialog", { name: /plays once/ });
    await expect(viewer).toBeVisible();
    await expect(viewer.locator("a[download]")).toHaveCount(0);
    await expect.poll(() => server.store.get(mediaId).state).toBe("opened");
    expect(server.store.get(mediaId).parts.size).toBe(0);
    await viewer.getByRole("button", { name: /^Close / }).click();
    await expect(viewer).toHaveCount(0);
    await expect(bubble).toHaveAttribute("data-state", "opened");
    await expect(bubble).toBeDisabled();
    expect(await page.evaluate(() => window.harness.revoked.length)).toBeGreaterThan(0);
    expect(await page.evaluate(() => window.harness.received.viewOnce.key)).toBe("");

    const gone = await page.evaluate(async (value) => {
      try {
        await window.harness.client.download(value.conversation, value.viewOnce);
        return 0;
      } catch (error) {
        return error.status;
      }
    }, { conversation: CONVERSATION, viewOnce: frame.viewOnce });
    expect(gone).toBe(410);
    expect(server.calls).toContain("POST /api/chat/direct-messages/ID/media/ID/consume");
    expect(errors).toEqual([]);
    await context.close();
  });
}

test("holding the record button sends a voice message in a changed voice", async () => {
  test.setTimeout(60_000);
  const context = await browser.newContext({
    baseURL: "http://127.0.0.1:4179",
    permissions: ["camera", "microphone"],
  });
  const page = await context.newPage();
  const errors = [];
  page.on("pageerror", (error) => errors.push(error.message));
  const server = mediaServer();
  await page.route("**/api/chat/direct-messages/**", server.handle);
  await page.route("**/view-once-harness", (route) =>
    route.fulfill({ contentType: "text/html", body: HARNESS }));
  await page.goto("/view-once-harness");
  await page.waitForFunction(() => window.harnessReady === true);
  await page.evaluate(() => window.harness.useVoices());
  const framePromise = page.evaluate(() => window.harness.record("voice"));
  const dialog = page.getByRole("dialog");
  const picker = dialog.getByRole("combobox", { name: "Voice for this message" });
  await expect(picker).toHaveValue("");
  await picker.selectOption("maya");
  const record = dialog.getByRole("button", { name: /Start recording/ });
  const box = await record.boundingBox();
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.down();
  await expect(dialog.getByRole("button", { name: "Stop recording" })).toBeVisible();
  await expect(picker).toBeDisabled();
  await page.waitForTimeout(2000);
  await page.mouse.up();
  const frame = await framePromise;
  expect(frame.viewOnce.kind).toBe("voice");
  expect(frame.viewOnce.durationMs).toBeGreaterThan(1200);
  expect([...server.store.values()][0].state).toBe("ready");
  await expect(dialog).toHaveCount(0);
  expect(errors).toEqual([]);
  await context.close();
});

test("chat page: a direct message carries a voice message that plays once and reports Opened", async () => {
  test.setTimeout(90_000);
  const relay = [];
  const server = mediaServer();
  const receipts = [];
  const conversationFor = (other) => ({
    id: CONVERSATION, otherUser: other, updatedAt: 1, keyVersion: 1, unreadCount: 0,
  });
  async function open(name, other) {
    const context = await browser.newContext({
      baseURL: "http://127.0.0.1:4179",
      permissions: ["camera", "microphone"],
    });
    await context.addInitScript((session) => {
      localStorage.setItem("forkmesh.session", JSON.stringify(session));
    }, { kind: "user", nodeName: name, email: `${name}@example.test`, sessionToken: `token-${name}`, status: "active" });
    const page = await context.newPage();
    const errors = [];
    page.on("pageerror", (error) => errors.push(error.message));
    await page.route("**/api/**", async (route) => {
      const url = new URL(route.request().url());
      if (url.pathname.startsWith(BASE)) {
        const result = await server.handle(route);
        if (url.pathname.endsWith("/consume")) {
          const [id] = url.pathname.match(/[0-9a-f]{32}(?=\/consume)/);
          for (const peer of relay) {
            peer.socket.send(JSON.stringify({ kind: "forkmesh-media", v: 1, id, state: "opened" }));
          }
        }
        return result;
      }
      let body = { error: "not_found" };
      let status = 404;
      if (url.pathname === "/api/chat/direct-messages") {
        status = 200;
        body = route.request().method() === "POST"
          ? { ok: true, conversation: conversationFor(other) }
          : { conversations: [conversationFor(other)], nextCursor: "" };
      } else if (url.pathname === `/api/chat/direct-messages/${CONVERSATION}/room-access`) {
        status = 200;
        body = {
          room: `chat-direct:${CONVERSATION}:v1`,
          passphrase: "playwright-direct-passphrase",
          webSocketUrl: `/api/chat/direct-messages/${CONVERSATION}/ws?ticket=${name}`,
          conversation: conversationFor(other),
        };
      } else if (url.pathname.endsWith("/read")) {
        status = 200;
        body = { ok: true };
      } else if (url.pathname === "/api/accounts/users") {
        status = 200;
        body = { ok: true, users: [{ name: other }, { name }] };
      }
      return route.fulfill({ status, contentType: "application/json", body: JSON.stringify(body) });
    });
    await page.routeWebSocket(/\/api\/chat\/direct-messages\/[0-9a-f]{32}\/ws/, (socket) => {
      const peer = { name, socket };
      relay.push(peer);
      socket.onMessage((message) => {
        const text = String(message);
        const frame = JSON.parse(text);
        if (frame.kind === "forkmesh-received") {
          receipts.push({ name, refs: frame.refs });
          return;
        }
        if (frame.kind && frame.kind !== "cipher" && !frame.iv) return;
        const ref = createHash("sha256").update(text).digest("hex").slice(0, 40);
        const relayed = JSON.stringify({ ...frame, ref });
        for (const other of relay) {
          if (other !== peer) other.socket.send(relayed);
        }
      });
      setTimeout(() => socket.send(JSON.stringify({ kind: "forkmesh-history-end", v: 1 })), 20);
    });
    await page.goto(`/chat.html?dm=${other}`);
    await expect(page.locator("#chat-status")).toContainText("Connected");
    return { context, page, errors };
  }

  const bob = await open("bob", "alice");
  const alice = await open("alice", "bob");
  const voiceButton = alice.page.getByRole("button", { name: /Record a voice message/ });
  await expect(voiceButton).toBeVisible();
  await voiceButton.click();
  const dialog = alice.page.getByRole("dialog");
  await dialog.getByRole("button", { name: /Start recording/ }).click();
  await alice.page.waitForTimeout(2000);
  await dialog.getByRole("button", { name: "Stop recording" }).click();
  await dialog.getByRole("button", { name: /^Send / }).click();
  await expect(dialog).toHaveCount(0);

  const sent = alice.page.locator("#chat-log .chat-view-once");
  await expect(sent).toHaveAttribute("data-state", "sent");
  await expect(sent).toBeDisabled();
  const received = bob.page.locator("#chat-log .chat-view-once");
  await expect(received).toBeVisible({ timeout: 20_000 });
  await expect(received).toHaveAttribute("data-state", "ready");
  await expect(bob.page.locator("#chat-log .chat-msg", { has: received })
    .getByRole("button", { name: "Copy text" })).toHaveCount(0);
  expect(receipts.filter((entry) => entry.name === "bob")).toEqual([]);

  await received.click();
  const viewer = bob.page.getByRole("dialog", { name: /plays once/ });
  await expect(viewer).toBeVisible();
  await expect.poll(() => [...server.store.values()][0]?.state).toBe("opened");
  await expect(sent).toHaveAttribute("data-state", "opened");
  await viewer.getByRole("button", { name: /^Close / }).click();
  await expect(received).toHaveAttribute("data-state", "opened");
  await expect(received).toBeDisabled();
  await expect.poll(() => receipts.filter((entry) => entry.name === "bob").length).toBe(1);
  expect([...server.store.values()][0].parts.size).toBe(0);
  expect(alice.errors).toEqual([]);
  expect(bob.errors).toEqual([]);
  await alice.context.close();
  await bob.context.close();
});

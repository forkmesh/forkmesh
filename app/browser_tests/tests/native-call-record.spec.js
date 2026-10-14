const { test, expect, chromium } = require("@playwright/test");
const path = require("node:path");

const PAGE = path.resolve(__dirname, "..", "..", "..", "native", "ios", "ForkMesh", "Features", "Chat", "Calls", "direct-call.html");
const URL_PAGE = "http://127.0.0.1:4179/__native/direct-call.html";

const BRIDGE = `
window.__results = new Map();
window.__waiters = new Map();
window.FmCallBridge = {
  post(text) {
    const message = JSON.parse(text);
    if (message.event !== "result") return;
    const waiter = window.__waiters.get(message.id);
    if (waiter) {
      window.__waiters.delete(message.id);
      waiter(message);
    } else {
      window.__results.set(message.id, message);
    }
  },
};
let nextCall = 100;
window.callOp = (name, args) => new Promise((resolve) => {
  const id = ++nextCall;
  window.__waiters.set(id, resolve);
  window.fmCall(id, name, args || []);
});
`;

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

async function openPage() {
  const context = await browser.newContext({ permissions: ["camera", "microphone"] });
  const page = await context.newPage();
  const errors = [];
  page.on("pageerror", (error) => errors.push(error.message));
  await page.addInitScript(BRIDGE);
  await page.route(URL_PAGE, (route) => route.fulfill({ path: PAGE, contentType: "text/html" }));
  await page.goto(URL_PAGE);
  await page.waitForFunction(() => typeof window.fmCall === "function");
  return { context, page, errors };
}

const call = (page, name, args) => page.evaluate(([op, list]) => window.callOp(op, list), [name, args || []]);

test("records the previewed picture and voice, hands the file back in chunks, then forgets it", async () => {
  test.setTimeout(60_000);
  const { context, page, errors } = await openPage();
  const voices = await call(page, "voiceList");
  expect(voices.ok).toBe(true);
  const voice = voices.value.length ? voices.value[0].id : "";
  const preview = await call(page, "preview", ["video", voice, "", "", ""]);
  expect(preview.ok).toBe(true);
  expect(preview.value).toBe(voice);

  const refused = await call(page, "record", ["maya", "", voice]);
  expect(refused).toMatchObject({ ok: false, error: "face_unavailable" });
  const wrongVoice = await call(page, "record", ["", "", voice ? "" : "maya"]);
  expect(wrongVoice).toMatchObject({ ok: false, error: "voice_unavailable" });

  const started = await call(page, "record", ["", "", voice]);
  expect(started).toMatchObject({ ok: true, value: true });
  await page.waitForTimeout(2500);
  const stopped = await call(page, "recordStop");
  expect(stopped.ok).toBe(true);
  expect(stopped.value.mime).toMatch(/^video\//);
  expect(stopped.value.durationMs).toBeGreaterThan(2000);
  expect(stopped.value.size).toBeGreaterThan(1000);

  let bytes = Buffer.alloc(0);
  while (bytes.length < stopped.value.size) {
    const chunk = await call(page, "recordChunk", [bytes.length]);
    expect(chunk.ok).toBe(true);
    const part = Buffer.from(chunk.value, "base64");
    expect(part.length).toBeGreaterThan(0);
    bytes = Buffer.concat([bytes, part]);
  }
  expect(bytes.length).toBe(stopped.value.size);
  const mp4 = bytes.subarray(4, 8).toString("latin1") === "ftyp";
  const webm = bytes.subarray(0, 4).equals(Buffer.from([0x1a, 0x45, 0xdf, 0xa3]));
  expect(mp4 || webm).toBe(true);

  const decoded = await page.evaluate(async ([text, type]) => {
    const raw = Uint8Array.from(atob(text), (c) => c.charCodeAt(0));
    const video = document.createElement("video");
    video.muted = true;
    video.src = URL.createObjectURL(new Blob([raw], { type }));
    await new Promise((resolve, reject) => {
      video.onloadeddata = resolve;
      video.onerror = () => reject(new Error("decode"));
    });
    return { width: video.videoWidth, height: video.videoHeight };
  }, [bytes.toString("base64"), stopped.value.mime]);
  expect(decoded.width).toBeGreaterThan(0);
  expect(decoded.height).toBeGreaterThan(0);

  expect(await call(page, "recordDrop")).toMatchObject({ ok: true });
  expect(await call(page, "recordChunk", [0])).toMatchObject({ ok: false, error: "not_recorded" });
  expect(await call(page, "recordStop")).toMatchObject({ ok: false, error: "not_recording" });
  expect(errors).toEqual([]);
  await context.close();
});

test("waits for the background to be ready and records the stage, not the camera", async () => {
  test.setTimeout(60_000);
  const { context, page, errors } = await openPage();
  const voices = await call(page, "voiceList");
  expect(voices.value.length).toBeGreaterThan(0);
  const preview = await call(page, "preview", ["video", "", "", "blur", ""]);
  expect(preview.ok).toBe(true);
  const started = await call(page, "record", ["", "blur", ""]);
  expect(started).toMatchObject({ ok: true, value: true });
  const recorded = await page.evaluate(() => {
    const local = document.getElementById("local");
    const track = local.srcObject && local.srcObject.getVideoTracks()[0];
    return { label: track ? track.label : "" };
  });
  expect(recorded.label).not.toMatch(/fake/i);
  await page.waitForTimeout(1500);
  const stopped = await call(page, "recordStop");
  expect(stopped.ok).toBe(true);
  expect(stopped.value.size).toBeGreaterThan(1000);
  await call(page, "close");
  expect(await call(page, "recordChunk", [0])).toMatchObject({ ok: false, error: "not_recorded" });
  expect(errors).toEqual([]);
  await context.close();
});

test("refuses to record before the preview is open or once the call has started", async () => {
  const { context, page } = await openPage();
  expect(await call(page, "record", ["", "", ""])).toMatchObject({ ok: false, error: "not_previewing" });
  await call(page, "preview", ["video", "", "", "", ""]);
  await call(page, "setup", ["video", [], "", ""]);
  expect(await call(page, "record", ["", "", ""])).toMatchObject({ ok: false, error: "not_previewing" });
  await call(page, "close");
  await context.close();
});
